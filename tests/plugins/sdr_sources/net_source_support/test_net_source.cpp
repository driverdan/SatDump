// These tests need POSIX sockets
#include "sdr_sources/net_source_support/net_source.h"
#include "common/dsp/buffer.h"
#include "helpers/guards.h"
#include "helpers/sockets.h"
#include "helpers/wait.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The network source is run against a UDP sender and a nng publisher made by the test

#ifndef _WIN32
#include "common/net/udp.h"
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>

using test_helpers::waitUntil;

namespace
{
    // Stopping has to take about one receive timeout, this leaves plenty of margin
    constexpr std::chrono::milliseconds kStopTimeout(3000);

    std::shared_ptr<dsp::DSPSampleSource> makeSource(nlohmann::json settings)
    {
        auto source = NetSource::getInstance(NetSource::getAvailableSources()[0]);
        source->set_settings(settings);
        source->set_samplerate(1000000);
        source->open();
        return source;
    }

    // Runs f on its own thread and returns true if it returns in time. A thread that does
    // not is detached (with what it holds), so that a hang fails the test instead of
    // stalling the run.
    bool returnsInTime(std::function<void()> f)
    {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread t(
            [f, done]()
            {
                f();
                *done = true;
            });
        bool returned = waitUntil([&]() { return done->load(); }, kStopTimeout);
        if (returned)
            t.join();
        else
            t.detach();
        return returned;
    }

    // Stops the source, then destroys it (which joins its thread), both in time
    bool stopsAndClosesInTime(std::shared_ptr<dsp::DSPSampleSource> &source)
    {
        auto s = source;
        source.reset();
        return returnsInTime(
            [s]() mutable
            {
                s->stop();
                s->close();
                s.reset();
            });
    }

    // Reads the output stream of a started source, like the live pipeline does
    struct Reader
    {
        std::shared_ptr<dsp::stream<complex_t>> stream;
        std::mutex mtx;
        std::vector<std::vector<complex_t>> reads;
        std::thread t;

        explicit Reader(std::shared_ptr<dsp::stream<complex_t>> s) : stream(s)
        {
            t = std::thread(
                [this]()
                {
                    int n;
                    while ((n = stream->read()) > 0)
                    {
                        {
                            std::lock_guard<std::mutex> lock(mtx);
                            reads.emplace_back(stream->readBuf, stream->readBuf + n);
                        }
                        stream->flush();
                    }
                });
        }

        size_t count()
        {
            std::lock_guard<std::mutex> lock(mtx);
            return reads.size();
        }

        std::vector<std::vector<complex_t>> get()
        {
            std::lock_guard<std::mutex> lock(mtx);
            return reads;
        }

        ~Reader()
        {
            stream->stopReader();
            t.join();
        }
    };

    std::vector<complex_t> samples(int n, float base)
    {
        std::vector<complex_t> v;
        for (int i = 0; i < n; i++)
            v.push_back(complex_t(base + i, -(base + i)));
        return v;
    }

    std::vector<uint8_t> bytesOf(const std::vector<complex_t> &v, int extra_bytes = 0)
    {
        std::vector<uint8_t> b(v.size() * sizeof(complex_t) + extra_bytes, 0x55);
        memcpy(b.data(), v.data(), v.size() * sizeof(complex_t));
        return b;
    }

    bool sameSamples(const std::vector<complex_t> &a, const std::vector<complex_t> &b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
            if (a[i].real != b[i].real || a[i].imag != b[i].imag)
                return false;
        return true;
    }

    // A free UDP port: bound to an ephemeral port, then closed
    int freeUdpPort()
    {
        int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(s, (sockaddr *)&addr, sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(s, (sockaddr *)&addr, &len);
        close(s);
        return ntohs(addr.sin_port);
    }

    // A nng publisher listening on 127.0.0.1, like NetworkServerModule in default mode
    struct NngPublisher
    {
        nng_socket sock;
        int port = 0;
        bool ok = false;

        NngPublisher()
        {
            if (nng_pub0_open_raw(&sock) != 0)
                return;
            for (int attempt = 0; attempt < 10 && !ok; attempt++)
            {
                port = test_helpers::closedPort();
                ok = nng_listen(sock, ("tcp://127.0.0.1:" + std::to_string(port)).c_str(), nullptr, 0) == 0;
            }
        }

        ~NngPublisher() { nng_close(sock); }

        void send(std::vector<uint8_t> data) { nng_send(sock, data.data(), data.size(), NNG_FLAG_NONBLOCK); }
    };
} // namespace

TEST_CASE("NetSource stops promptly in UDP mode when no datagram arrives", "[sdr_sources][net_source]")
{
    auto source = makeSource({{"mode", "udp"}, {"port", freeUdpPort()}});

    // Twice, so a restart works too
    for (int i = 0; i < 2; i++)
    {
        source->start();
        Reader reader(source->output_stream);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK_FALSE(source->has_ended());
        CHECK(returnsInTime([&]() { source->stop(); }));
        CHECK(reader.count() == 0);
    }

    source->start();
    CHECK(stopsAndClosesInTime(source));
}

TEST_CASE("NetSource outputs the UDP datagrams it receives", "[sdr_sources][net_source]")
{
    int port = freeUdpPort();
    auto source = makeSource({{"mode", "udp"}, {"port", port}});
    source->start();
    Reader reader(source->output_stream);

    char address[] = "127.0.0.1";
    net::UDPClient client(address, port);

    // A datagram shorter than a sample outputs nothing, a partial sample is dropped
    auto expected = samples(4, 1.0f);
    auto short_one = std::vector<uint8_t>(3, 0x11);
    auto data = bytesOf(expected, 3);
    bool received = waitUntil(
        [&]()
        {
            client.send(short_one.data(), short_one.size());
            client.send(data.data(), data.size());
            return reader.count() >= 2;
        },
        test_helpers::kTimeout, std::chrono::milliseconds(20));
    REQUIRE(received);

    CHECK(stopsAndClosesInTime(source));
    for (auto &r : reader.get())
        CHECK(sameSamples(r, expected));
}

TEST_CASE("NetSource stops promptly in NNG mode when the server sends nothing", "[sdr_sources][net_source]")
{
    NngPublisher server;
    REQUIRE(server.ok);

    auto source = makeSource({{"mode", "nng_sub"}, {"address", "127.0.0.1"}, {"port", server.port}});
    for (int i = 0; i < 2; i++)
    {
        source->start();
        Reader reader(source->output_stream);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // Still receiving, not ended by an error
        CHECK_FALSE(source->has_ended());
        CHECK(returnsInTime([&]() { source->stop(); }));
        CHECK(reader.count() == 0);
    }

    source->start();
    CHECK(stopsAndClosesInTime(source));
}

TEST_CASE("NetSource stops promptly in NNG mode when no server is listening", "[sdr_sources][net_source]")
{
    auto source = makeSource({{"mode", "nng_sub"}, {"address", "127.0.0.1"}, {"port", test_helpers::closedPort()}});

    // The dialer retries in the background
    REQUIRE_NOTHROW(source->start());
    Reader reader(source->output_stream);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(source->has_ended());
    CHECK(stopsAndClosesInTime(source));
    CHECK(reader.count() == 0);
}

TEST_CASE("NetSource outputs the NNG messages it receives", "[sdr_sources][net_source]")
{
    NngPublisher server;
    REQUIRE(server.ok);

    auto source = makeSource({{"mode", "nng_sub"}, {"address", "127.0.0.1"}, {"port", server.port}});
    source->start();
    Reader reader(source->output_stream);

    // The subscriber connects asynchronously: publish until something arrives
    auto expected = samples(4, 10.0f);
    bool received = waitUntil(
        [&]()
        {
            server.send(bytesOf(expected, 3));
            return reader.count() >= 2;
        },
        test_helpers::kTimeout, std::chrono::milliseconds(20));
    REQUIRE(received);

    CHECK(stopsAndClosesInTime(source));
    for (auto &r : reader.get())
        CHECK(sameSamples(r, expected));
}

TEST_CASE("NetSource truncates a NNG message larger than its buffer", "[sdr_sources][net_source]")
{
    NngPublisher server;
    REQUIRE(server.ok);

    // The output stream is sized from it when the source starts. The guard is declared
    // first so that it outlives the source.
    constexpr int kBufferSize = 16;
    test_helpers::ValueGuard buffer_size(dsp::STREAM_BUFFER_SIZE, kBufferSize);

    auto source = makeSource({{"mode", "nng_sub"}, {"address", "127.0.0.1"}, {"port", server.port}});
    source->start();
    Reader reader(source->output_stream);

    auto sent = samples(kBufferSize * 4, 100.0f);
    bool received = waitUntil(
        [&]()
        {
            server.send(bytesOf(sent));
            return reader.count() >= 2;
        },
        test_helpers::kTimeout, std::chrono::milliseconds(20));
    REQUIRE(received);

    CHECK(stopsAndClosesInTime(source));
    auto expected = std::vector<complex_t>(sent.begin(), sent.begin() + kBufferSize);
    for (auto &r : reader.get())
        CHECK(sameSamples(r, expected));
}
#endif
