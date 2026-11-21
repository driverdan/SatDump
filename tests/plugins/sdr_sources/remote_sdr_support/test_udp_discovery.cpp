#include "helpers/sockets.h"
#include "helpers/wait.h"
#include "sdr_sources/remote_sdr_support/remote.h"
#include "sdr_sources/remote_sdr_support/udp_discovery.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <thread>

// The discovery of Remote SDR servers is run on ports picked by the test rather than the real
// ones, so that it neither sees real servers nor gets in the way of a running SatDump.

#ifndef _WIN32
namespace
{
    using Servers = std::vector<std::pair<std::string, int>>;

    service_discovery::UDPDiscoveryConfig testConfig() { return {test_helpers::closedPort(), test_helpers::closedPort(), REMOTE_NETWORK_DISCOVERY_REQPKT, REMOTE_NETWORK_DISCOVERY_REPPKT}; }

    // A discoverUDPServers() call on its own thread. The thread is detached, so that a call that
    // never returns fails the test instead of hanging it.
    struct Discovery
    {
        std::atomic<bool> done = false;
        Servers servers;
    };

    std::shared_ptr<Discovery> startDiscovery(service_discovery::UDPDiscoveryConfig cfg, int wait_millis)
    {
        auto d = std::make_shared<Discovery>();
        std::thread(
            [d, cfg, wait_millis]()
            {
                d->servers = service_discovery::discoverUDPServers(cfg, wait_millis);
                d->done = true;
            })
            .detach();
        return d;
    }

    void sendTo(int port, std::vector<uint8_t> pkt)
    {
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(fd, pkt.data(), pkt.size(), 0, (sockaddr *)&addr, sizeof(addr));
        close(fd);
    }
} // namespace

TEST_CASE("discoverUDPServers lists the servers that reply", "[plugins][remote_sdr_support][udp_discovery]")
{
    auto cfg = testConfig();
    auto d = startDiscovery(cfg, 500);

    // A reply is the reply packet followed by the TCP port of the server, big endian
    std::vector<uint8_t> reply = cfg.rep_pkt;
    reply.insert(reply.end(), {0x00, 0x00, 0x16, 0x2E}); // 5678
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    sendTo(cfg.rep_port, reply);

    REQUIRE(test_helpers::waitUntil([&]() { return d->done.load(); }));
    REQUIRE(d->servers.size() == 1);
    CHECK(d->servers[0].first == "127.0.0.1");
    CHECK(d->servers[0].second == 5678);
}

// Two discoveries on the same port, as when two satdump instances start together. Each one
// ends by sending a packet to its own socket on 127.0.0.1 to wake it up, but a unicast packet
// only reaches one of the sockets bound to a port: the first one to finish can lose its packet
// to the other one, which is still waiting and ignores it.
TEST_CASE("discoverUDPServers returns while another discovery uses the same port", "[plugins][remote_sdr_support][udp_discovery]")
{
    auto cfg = testConfig();

    auto first = startDiscovery(cfg, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto second = startDiscovery(cfg, 400);

    CHECK(test_helpers::waitUntil([&]() { return first->done.load(); }, std::chrono::milliseconds(5000)));
    CHECK(test_helpers::waitUntil([&]() { return second->done.load(); }, std::chrono::milliseconds(5000)));
}
#endif
