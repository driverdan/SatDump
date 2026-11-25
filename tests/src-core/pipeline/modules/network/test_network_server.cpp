// These tests need POSIX sockets
#include "pipeline/modules/network/module_network_server.h"
#include "core/exception.h"
#include "helpers/sockets.h"
#include "helpers/temp_files.h"
#include "helpers/wait.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include "common/net/udp.h"
#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>

namespace pl = satdump::pipeline;
using test_helpers::waitUntil;

namespace
{
    constexpr int kPktSize = 16;

    std::shared_ptr<pl::ProcessingModule> makeServer(std::string input_file, nlohmann::json params)
    {
        params["pkt_size"] = kPktSize;
        params["server_address"] = "127.0.0.1";
        auto m = pl::network::NetworkServerModule::getInstance(input_file, "", params);
        m->setInputType(pl::DATA_FILE);
        m->setOutputType(pl::DATA_FILE);
        return m;
    }

    // Runs process() on its own thread, so that a hang fails the test instead of stalling
    // the run. Returns true if it returned in time, and whether it threw.
    bool runBounded(std::shared_ptr<pl::ProcessingModule> m, bool &threw)
    {
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto thrown = std::make_shared<std::atomic<bool>>(false);
        std::thread t(
            [m, done, thrown]()
            {
                try
                {
                    m->process();
                }
                catch (satdump::satdump_exception_t &)
                {
                    *thrown = true;
                }
                *done = true;
            });
        bool returned = waitUntil([&]() { return done->load(); });
        if (returned)
            t.join();
        else
            t.detach();
        threw = thrown->load();
        return returned;
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

    // A nng socket listening on 127.0.0.1, holding the port
    struct NngListener
    {
        nng_socket sock;
        int port = 0;
        bool ok = false;

        NngListener()
        {
            if (nng_sub0_open_raw(&sock) != 0)
                return;
            for (int attempt = 0; attempt < 10 && !ok; attempt++)
            {
                port = test_helpers::closedPort();
                ok = nng_listen(sock, ("tcp://127.0.0.1:" + std::to_string(port)).c_str(), nullptr, 0) == 0;
            }
        }

        ~NngListener() { nng_close(sock); }
    };
} // namespace

TEST_CASE("NetworkServerModule requires pkt_size, server_address and server_port", "[pipeline][modules][network_server]")
{
    CHECK_THROWS_AS(pl::network::NetworkServerModule("", "", {{"server_address", "127.0.0.1"}, {"server_port", 1234}}), satdump::satdump_exception_t);
    CHECK_THROWS_AS(pl::network::NetworkServerModule("", "", {{"pkt_size", kPktSize}, {"server_port", 1234}}), satdump::satdump_exception_t);
    CHECK_THROWS_AS(pl::network::NetworkServerModule("", "", {{"pkt_size", kPktSize}, {"server_address", "127.0.0.1"}}), satdump::satdump_exception_t);
    CHECK_NOTHROW(pl::network::NetworkServerModule("", "", {{"pkt_size", kPktSize}, {"server_address", "127.0.0.1"}, {"server_port", 1234}}));
}

TEST_CASE("NetworkServerModule reports a TCP port already in use", "[pipeline][modules][network_server]")
{
    NngListener other;
    REQUIRE(other.ok);

    test_helpers::TempFile input("in.bin", std::string(kPktSize * 4, 'x'));
    auto m = makeServer(input.str(), {{"server_port", other.port}});

    bool threw = false;
    REQUIRE(runBounded(m, threw));
    CHECK(threw);
}

TEST_CASE("NetworkServerModule can serve a file over TCP repeatedly on the same port", "[pipeline][modules][network_server]")
{
    int port = test_helpers::closedPort();
    test_helpers::TempFile input("in.bin", std::string(kPktSize * 4, 'x'));

    // The port must be free again once a run is over
    for (int run = 0; run < 3; run++)
    {
        auto m = makeServer(input.str(), {{"server_port", port}});
        bool threw = true;
        REQUIRE(runBounded(m, threw));
        CHECK_FALSE(threw);
    }
}

TEST_CASE("NetworkServerModule reports an input file it cannot open", "[pipeline][modules][network_server]")
{
    auto missing = test_helpers::uniqueTempPath("missing.bin").string();

    for (std::string mode : {"default", "udp_send"})
    {
        auto m = makeServer(missing, {{"server_mode", mode}, {"server_port", freeUdpPort()}});
        bool threw = false;
        // Used to loop forever, as eof() is never set on a stream that failed to open
        REQUIRE(runBounded(m, threw));
        CHECK(threw);
    }
}

TEST_CASE("NetworkServerModule sends only the bytes left at the end of a file", "[pipeline][modules][network_server]")
{
    // Two full packets, then half a packet
    std::string content;
    for (int i = 0; i < kPktSize * 2 + kPktSize / 2; i++)
        content.push_back((char)(i < kPktSize ? 0xAA : (i < kPktSize * 2 ? 0xBB : 0xCC)));
    test_helpers::TempFile input("in.bin", content);

    int port = freeUdpPort();
    net::UDPServer receiver(port);
    receiver.enableTimeout();

    auto m = makeServer(input.str(), {{"server_mode", "udp_send"}, {"server_port", port}});
    bool threw = true;
    REQUIRE(runBounded(m, threw));
    CHECK_FALSE(threw);

    // Loopback UDP keeps the datagrams in order and does not drop them at this rate
    std::vector<std::vector<uint8_t>> received;
    std::vector<uint8_t> buf(kPktSize * 4);
    for (int i = 0; i < 10; i++)
    {
        try
        {
            int r = receiver.recv(buf.data(), buf.size());
            received.emplace_back(buf.begin(), buf.begin() + r);
        }
        catch (std::runtime_error &) // Receive timeout, nothing left
        {
            break;
        }
    }

    REQUIRE(received.size() == 3);
    CHECK(received[0] == std::vector<uint8_t>(kPktSize, 0xAA));
    CHECK(received[1] == std::vector<uint8_t>(kPktSize, 0xBB));
    CHECK(received[2] == std::vector<uint8_t>(kPktSize / 2, 0xCC));
}
#endif
