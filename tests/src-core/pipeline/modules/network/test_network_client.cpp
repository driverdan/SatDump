// These tests need POSIX sockets
#include "pipeline/modules/network/module_network_client.h"
#include "common/dsp/buffer.h"
#include "core/exception.h"
#include "helpers/sockets.h"
#include "helpers/wait.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include "common/net/udp.h"
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>

namespace pl = satdump::pipeline;
using test_helpers::waitUntil;

namespace
{
    constexpr int kPktSize = 16;

    // Stopping has to take about one receive timeout, this leaves plenty of margin
    constexpr std::chrono::milliseconds kStopTimeout(3000);

    std::shared_ptr<pl::ProcessingModule> makeClient(nlohmann::json params)
    {
        auto m = pl::network::NetworkClientModule::getInstance("", "", params);
        m->setInputType(pl::DATA_STREAM);
        m->setOutputType(pl::DATA_STREAM);
        m->output_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
        m->input_active = true;
        return m;
    }

    // Runs process() on its own thread, like LivePipeline does. A thread that does not
    // return in time is detached, so that a hang fails the test instead of stalling the run.
    struct ProcessThread
    {
        std::shared_ptr<pl::ProcessingModule> m;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
        std::thread t;

        explicit ProcessThread(std::shared_ptr<pl::ProcessingModule> mod) : m(mod)
        {
            auto d = done;
            t = std::thread(
                [mod, d]()
                {
                    try
                    {
                        mod->process();
                    }
                    catch (std::exception &)
                    {
                    }
                    *d = true;
                });
        }

        // Clears input_active, as LivePipeline::stop() does, and returns true if process() returns in time
        bool stop()
        {
            m->input_active = false;
            bool returned = waitUntil([this]() { return done->load(); }, kStopTimeout);
            if (returned)
                t.join();
            else
                t.detach();
            return returned;
        }

        ~ProcessThread()
        {
            if (t.joinable())
                stop();
        }
    };

    // Reads up to n bytes the module wrote, waiting for them for a bounded time
    std::vector<uint8_t> readOutput(pl::ProcessingModule &m, int n)
    {
        std::vector<uint8_t> out(n);
        if (!waitUntil([&]() { return m.output_fifo->getReadable() >= n; }))
            return {};
        m.output_fifo->read(out.data(), n);
        return out;
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

    std::vector<uint8_t> packet(uint8_t value, int size = kPktSize) { return std::vector<uint8_t>(size, value); }
} // namespace

TEST_CASE("NetworkClientModule requires pkt_size in both modes", "[pipeline][modules][network_client]")
{
    CHECK_THROWS_AS(pl::network::NetworkClientModule("", "", {{"server_address", "127.0.0.1"}, {"server_port", 1234}}), satdump::satdump_exception_t);
    CHECK_THROWS_AS(pl::network::NetworkClientModule("", "", {{"client_mode", "udp_listen"}, {"server_port", 1234}}), satdump::satdump_exception_t);
    CHECK_NOTHROW(pl::network::NetworkClientModule("", "", {{"client_mode", "udp_listen"}, {"pkt_size", kPktSize}, {"server_port", 1234}}));
}

TEST_CASE("NetworkClientModule stops promptly when a TCP server sends nothing", "[pipeline][modules][network_client]")
{
    NngPublisher server;
    REQUIRE(server.ok);

    auto m = makeClient({{"pkt_size", kPktSize}, {"server_address", "127.0.0.1"}, {"server_port", server.port}});
    ProcessThread run(m);

    // Let it connect and block in the receive
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(run.stop());
    CHECK(m->output_fifo->getReadable() == 0);
}

TEST_CASE("NetworkClientModule stops promptly when no TCP server is listening", "[pipeline][modules][network_client]")
{
    auto m = makeClient({{"pkt_size", kPktSize}, {"server_address", "127.0.0.1"}, {"server_port", test_helpers::closedPort()}});
    ProcessThread run(m);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(run.stop());
}

TEST_CASE("NetworkClientModule outputs the TCP packets of pkt_size only", "[pipeline][modules][network_client]")
{
    NngPublisher server;
    REQUIRE(server.ok);

    auto m = makeClient({{"pkt_size", kPktSize}, {"server_address", "127.0.0.1"}, {"server_port", server.port}});
    ProcessThread run(m);

    // The subscriber connects asynchronously: publish until something arrives.
    // A packet larger than the receive buffer must not overflow it.
    bool received = waitUntil(
        [&]()
        {
            server.send(packet(0xAA, kPktSize - 1));
            server.send(packet(0xBB, kPktSize * 20));
            server.send(packet(0xCC));
            return m->output_fifo->getReadable() >= kPktSize;
        },
        test_helpers::kTimeout, std::chrono::milliseconds(20));
    REQUIRE(received);

    CHECK(run.stop());
    CHECK(m->output_fifo->getReadable() % kPktSize == 0);
    while (m->output_fifo->getReadable() > 0)
        CHECK(readOutput(*m, kPktSize) == packet(0xCC));
}

TEST_CASE("NetworkClientModule stops promptly when no UDP datagram arrives", "[pipeline][modules][network_client]")
{
    auto m = makeClient({{"client_mode", "udp_listen"}, {"pkt_size", kPktSize}, {"server_port", freeUdpPort()}});
    ProcessThread run(m);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(run.stop());
    CHECK(m->output_fifo->getReadable() == 0);
}

TEST_CASE("NetworkClientModule outputs the UDP datagrams it receives", "[pipeline][modules][network_client]")
{
    int port = freeUdpPort();
    auto m = makeClient({{"client_mode", "udp_listen"}, {"pkt_size", kPktSize}, {"server_port", port}});
    ProcessThread run(m);

    char address[] = "127.0.0.1";
    net::UDPClient client(address, port);

    // The socket is bound on the module thread: send until something arrives
    auto data = packet(0x42);
    bool received = waitUntil(
        [&]()
        {
            client.send(data.data(), data.size());
            return m->output_fifo->getReadable() >= kPktSize;
        },
        test_helpers::kTimeout, std::chrono::milliseconds(20));
    REQUIRE(received);

    CHECK(run.stop());
    while (m->output_fifo->getReadable() > 0)
        CHECK(readOutput(*m, kPktSize) == data);
}
#endif
