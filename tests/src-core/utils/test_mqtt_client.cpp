#include "core/exception.h"
#include "helpers/sockets.h"
#include "utils/mqtt_client.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <memory>
#include <sys/resource.h>
#include <system_error>
#endif

// The only broker used here is a minimal fake on 127.0.0.1 run by the test itself. It answers the
// CONNECT with a CONNACK, records everything the client sends, and can push packets to the client.
// It uses POSIX sockets, so the tests needing it (or a closed port) are POSIX only.

#ifndef _WIN32
namespace
{
    using test_helpers::closedPort;

    const auto TIMEOUT = std::chrono::seconds(15);

    class FakeBroker
    {
    private:
        int listen_fd = -1;
        int client_fd = -1;
        std::thread th;
        std::atomic<bool> stop{false};
        bool connack_sent = false;

        void loop()
        {
            pollfd p{listen_fd, POLLIN, 0};
            auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
            while (!stop && std::chrono::steady_clock::now() < deadline)
                if (poll(&p, 1, 50) > 0)
                    break;
            if (stop || !(p.revents & POLLIN))
                return;
            int c = accept(listen_fd, nullptr, nullptr);
            if (c < 0)
                return;
            {
                std::lock_guard<std::mutex> l(mtx);
                client_fd = c;
            }

            char buf[2048];
            while (!stop)
            {
                pollfd pc{c, POLLIN, 0};
                if (poll(&pc, 1, 50) <= 0)
                    continue;
                ssize_t n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0)
                {
                    std::lock_guard<std::mutex> l(mtx);
                    peer_closed = true;
                    cv.notify_all();
                    break;
                }
                std::lock_guard<std::mutex> l(mtx);
                received.append(buf, n);
                // The first packet from the client is CONNECT (type 1)
                if (!connack_sent && (uint8_t)received[0] == 0x10)
                {
                    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
                    send(c, connack, sizeof(connack), MSG_NOSIGNAL);
                    connack_sent = true;
                }
                cv.notify_all();
            }
        }

    public:
        std::mutex mtx;
        std::condition_variable cv;
        std::string received;
        bool peer_closed = false;
        int port = 0;

        FakeBroker()
        {
            listen_fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t len = sizeof(addr);
            if (listen_fd < 0 || bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 1) != 0 || getsockname(listen_fd, (sockaddr *)&addr, &len) != 0)
                throw std::runtime_error("Could not set up fake broker");
            port = ntohs(addr.sin_port);
            th = std::thread(&FakeBroker::loop, this);
        }

        ~FakeBroker()
        {
            stop = true;
            th.join();
            if (client_fd >= 0)
                close(client_fd);
            close(listen_fd);
        }

        bool waitFor(const std::string &needle)
        {
            std::unique_lock<std::mutex> l(mtx);
            return cv.wait_for(l, TIMEOUT, [&] { return received.find(needle) != std::string::npos; });
        }

        bool waitClosed(std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> l(mtx);
            return cv.wait_for(l, timeout, [&] { return peer_closed; });
        }

        void sendToClient(const std::string &bytes)
        {
            std::lock_guard<std::mutex> l(mtx);
            if (client_fd >= 0)
                send(client_fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        }
    };

    std::string bin(std::initializer_list<int> v)
    {
        std::string s;
        for (int c : v)
            s.push_back((char)c);
        return s;
    }
} // namespace

TEST_CASE("MQTTClient throws when the broker refuses the connection", "[utils][mqtt]")
{
    CHECK_THROWS_AS(satdump::MQTTClient("127.0.0.1", std::to_string(closedPort()), 1024), satdump::satdump_exception_t);
}

#endif

TEST_CASE("MQTTClient throws on an unresolvable service", "[utils][mqtt]")
{
    // getaddrinfo() rejects the non-numeric, unknown port without any network lookup
    CHECK_THROWS_AS(satdump::MQTTClient("127.0.0.1", "satdump-no-such-service", 1024), satdump::satdump_exception_t);
}

#ifndef _WIN32
TEST_CASE("MQTTClient connects, publishes, subscribes and receives over loopback", "[utils][mqtt]")
{
    FakeBroker broker;

    std::mutex rx_mtx;
    std::condition_variable rx_cv;
    std::string rx_topic, rx_data;

    {
        satdump::MQTTClient client("127.0.0.1", std::to_string(broker.port), 4096,
                                   [&](std::string topic, uint8_t *data, int len)
                                   {
                                       std::lock_guard<std::mutex> l(rx_mtx);
                                       rx_topic = topic;
                                       rx_data = std::string((char *)data, len);
                                       rx_cv.notify_all();
                                   });

        // CONNECT: protocol name "MQTT", level 4, clean session, keep alive 400 s, empty client id
        REQUIRE(broker.waitFor(bin({0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x01, 0x90, 0x00, 0x00})));
        CHECK((uint8_t)broker.received[0] == 0x10);

        // QoS 0 PUBLISH: fixed header 0x30, remaining length 2 + 8 + 7, topic, payload
        std::string payload = "payload";
        client.publish("sd/topic", (uint8_t *)payload.data(), payload.size(), MQTT_PUBLISH_QOS_0);
        CHECK(broker.waitFor(bin({0x30, 17, 0x00, 0x08}) + "sd/topicpayload"));

        // SUBSCRIBE (0x82) carries the topic filter and requested QoS
        client.subscribe("sd/in/#", 1);
        CHECK(broker.waitFor(std::string("\x00\x07sd/in/#\x01", 10)));

        // Broker -> client PUBLISH reaches the callback
        broker.sendToClient(bin({0x30, 2 + 5 + 3, 0x00, 0x05}) + "sd/in" + "abc");
        {
            std::unique_lock<std::mutex> l(rx_mtx);
            REQUIRE(rx_cv.wait_for(l, TIMEOUT, [&] { return !rx_topic.empty(); }));
            CHECK(rx_topic == "sd/in");
            CHECK(rx_data == "abc");
        }
    }
}

// The destructor closes the socket. (Whether the queued DISCONNECT is sent at all is racy, as the
// refresher thread may stop before flushing it, so only the end of the connection is checked.)
TEST_CASE("MQTTClient destructor closes the broker connection", "[utils][mqtt]")
{
    FakeBroker broker;
    {
        satdump::MQTTClient client("127.0.0.1", std::to_string(broker.port), 1024);
        REQUIRE(broker.waitFor(std::string("\x00\x04MQTT", 6)));
    }
    CHECK(broker.waitClosed(std::chrono::milliseconds(1000)));
}

// Issue #212: when the constructor throws after opening the socket, the destructor does not run,
// so the constructor itself must close the socket (and free the buffers).
TEST_CASE("MQTTClient closes the socket when the constructor throws", "[utils][mqtt]")
{
    FakeBroker broker;
    // The TCP connection succeeds, but a 16-byte send buffer cannot hold the CONNECT packet
    CHECK_THROWS_AS(satdump::MQTTClient("127.0.0.1", std::to_string(broker.port), 16), satdump::satdump_exception_t);
    CHECK(broker.waitClosed(std::chrono::milliseconds(1000)));
}

TEST_CASE("MQTTClient closes the socket when the buffers cannot be allocated", "[utils][mqtt]")
{
    FakeBroker broker;
    // A bufsize of -1 asks malloc for SIZE_MAX bytes, so both buffers are null and mqtt_init fails
    CHECK_THROWS_AS(satdump::MQTTClient("127.0.0.1", std::to_string(broker.port), -1), satdump::satdump_exception_t);
    CHECK(broker.waitClosed(std::chrono::milliseconds(1000)));
}
#endif

#ifdef __linux__
// The refresher thread is created last in the constructor. If that throws, the constructor must
// still close the socket. Thread creation is made to fail by lowering RLIMIT_NPROC to 0 around
// the constructor, which is restored before any check. The listening socket is a bare one (no
// broker thread), as the kernel completes the TCP connection from the backlog without accept().
TEST_CASE("MQTTClient closes the socket when the refresher thread cannot start", "[utils][mqtt]")
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    REQUIRE(bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) == 0);
    REQUIRE(listen(listen_fd, 1) == 0);
    REQUIRE(getsockname(listen_fd, (sockaddr *)&addr, &len) == 0);
    const std::string port = std::to_string(ntohs(addr.sin_port));

    rlimit old_limit;
    REQUIRE(getrlimit(RLIMIT_NPROC, &old_limit) == 0);
    rlimit low_limit = old_limit;
    low_limit.rlim_cur = 0;

    bool threw = false;
    std::unique_ptr<satdump::MQTTClient> unexpected;
    if (setrlimit(RLIMIT_NPROC, &low_limit) == 0)
    {
        try
        {
            unexpected = std::make_unique<satdump::MQTTClient>("127.0.0.1", port, 1024);
        }
        catch (std::system_error &)
        {
            threw = true;
        }
        catch (...)
        {
            setrlimit(RLIMIT_NPROC, &old_limit);
            close(listen_fd);
            throw;
        }
        setrlimit(RLIMIT_NPROC, &old_limit);
    }
    else
    {
        close(listen_fd);
        SKIP("setrlimit(RLIMIT_NPROC) failed");
    }

    if (!threw)
    {
        // Privileged users are exempt from RLIMIT_NPROC, so the thread may start anyway
        unexpected.reset();
        close(listen_fd);
        SKIP("Thread creation did not fail under RLIMIT_NPROC = 0");
    }

    // The client never sent anything, so the first recv on the accepted side sees the close
    int c = accept(listen_fd, nullptr, nullptr);
    REQUIRE(c >= 0);
    pollfd p{c, POLLIN, 0};
    char buf[16];
    bool closed = poll(&p, 1, 1000) > 0 && recv(c, buf, sizeof(buf), 0) == 0;
    close(c);
    close(listen_fd);
    CHECK(closed);
}
#endif
