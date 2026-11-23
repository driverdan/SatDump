#pragma once

// Shared helpers for the tests that need a rtl_tcp server. Everything lives in the
// rtltcp_test namespace and is inline, so including this header from several translation
// units is fine. POSIX sockets only, so the whole header is empty on Windows: wrap its users
// in #ifndef _WIN32 as well.

#ifndef _WIN32
#include "helpers/wait.h"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <ostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace rtltcp_test
{
    // Command numbers of the rtl_tcp protocol
    enum : uint8_t
    {
        CMD_FREQUENCY = 1,
        CMD_SAMPLERATE = 2,
        CMD_GAIN_MODE = 3, // 1 is manual, 0 is the tuner AGC
        CMD_GAIN = 4,      // Tenths of dB
        CMD_PPM = 5,
        CMD_AGC_MODE = 8, // The AGC of the RTL2832
        CMD_BIAS_TEE = 14,
    };

    struct Command
    {
        uint8_t cmd;
        uint32_t param;

        bool operator==(const Command &o) const { return cmd == o.cmd && param == o.param; }
    };

    // So that a failed comparison shows the commands
    inline std::ostream &operator<<(std::ostream &os, const Command &c) { return os << "{" << (int)c.cmd << ", " << c.param << "}"; }

    // What rtl_tcp sends first: "RTL0", the tuner type (5 is a R820T) and its number of gains
    inline std::vector<uint8_t> dongleInfo() { return {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, 29}; }

    // What the server does once it has sent `iq`
    enum class AfterIQ
    {
        Silence, // Sends silence for as long as the client stays connected, as a dongle does
        Close,   // Closes the connection, as rtl_tcp does when it exits
        Reset,   // Resets the connection, the client gets a read error
        Stall,   // Sends nothing more but keeps the connection open, as when the network drops
    };

    // A rtl_tcp server on a free 127.0.0.1 port, for one client. It sends the dongle
    // information, then `iq` (unsigned 8-bit I/Q), then by default silence for as long as the
    // client stays connected, as a dongle never stops sending. The commands of the client are
    // recorded. Every blocking step is bounded, and the destructor stops everything.
    //
    // With a samplerate the samples are sent at that rate, as a dongle does. Without one they
    // are sent as fast as the client reads them.
    class FakeRtlTcp
    {
    private:
        int listen_fd = -1;
        int listen_port = 0;
        std::vector<uint8_t> data;
        size_t iq_start = 0;
        double bytes_per_second = 0;
        AfterIQ after_iq = AfterIQ::Silence;

        std::thread th;
        std::atomic<bool> should_stop{false};
        std::atomic<bool> client_connected{false};
        std::atomic<bool> client_left{false};
        std::atomic<uint64_t> sent{0};

        std::mutex mtx;
        std::vector<Command> received;

        void serve()
        {
            int fd = -1;
            while (!should_stop && fd < 0)
            {
                pollfd p{listen_fd, POLLIN, 0};
                if (poll(&p, 1, 20) > 0)
                    fd = accept(listen_fd, nullptr, nullptr);
            }
            if (fd < 0)
                return;
            client_connected = true;

            // Silence: the middle of the range, with the last bit toggling
            std::vector<uint8_t> silence(8192);
            for (size_t i = 0; i < silence.size(); i++)
                silence[i] = 127 + ((i * 2654435761u) >> 31);

            std::vector<uint8_t> pending; // Bytes of a command that is not complete yet
            size_t pos = 0, silence_pos = 0;
            uint64_t total = 0;
            auto start = std::chrono::steady_clock::now();
            auto last_input = start;
            while (!should_stop)
            {
                // Once `iq` was sent, the commands the client sends at start are read before the
                // connection is ended: unread data would turn the close into a reset
                bool iq_done = after_iq != AfterIQ::Silence && pos >= data.size();
                if (iq_done && after_iq != AfterIQ::Stall && std::chrono::steady_clock::now() - last_input >= std::chrono::milliseconds(100))
                {
                    if (after_iq == AfterIQ::Reset)
                    {
                        // Closing with a zero linger time sends a RST instead of a FIN
                        linger l{1, 0};
                        setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
                    }
                    break;
                }

                // Not ahead of the samplerate
                bool may_send = !iq_done;
                if (may_send && bytes_per_second > 0)
                    may_send = total < std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() * bytes_per_second;

                pollfd p{fd, (short)(may_send ? POLLIN | POLLOUT : POLLIN), 0};
                if (poll(&p, 1, may_send ? 20 : 1) <= 0)
                    continue;

                if (p.revents & (POLLIN | POLLHUP | POLLERR))
                {
                    uint8_t buf[256];
                    ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
                        break; // The client left
                    if (n > 0)
                    {
                        last_input = std::chrono::steady_clock::now();
                        pending.insert(pending.end(), buf, buf + n);
                        std::lock_guard<std::mutex> l(mtx);
                        while (pending.size() >= 5)
                        {
                            received.push_back({pending[0], (uint32_t)pending[1] << 24 | (uint32_t)pending[2] << 16 | (uint32_t)pending[3] << 8 | (uint32_t)pending[4]});
                            pending.erase(pending.begin(), pending.begin() + 5);
                        }
                    }
                }

                if (p.revents & POLLOUT)
                {
                    const uint8_t *from = pos < data.size() ? &data[pos] : &silence[silence_pos];
                    size_t len = pos < data.size() ? std::min<size_t>(data.size() - pos, 65536) : silence.size() - silence_pos;
                    ssize_t n = send(fd, from, len, MSG_DONTWAIT | MSG_NOSIGNAL);
                    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                        break;
                    if (n > 0)
                    {
                        total += n;
                        if (pos < data.size())
                            pos += n;
                        else
                            silence_pos = (silence_pos + n) % silence.size();
                        sent = pos > iq_start ? pos - iq_start : 0;
                    }
                }
            }

            close(fd);

            // As rtl_tcp exiting, the port is closed too: connecting again is refused
            if (after_iq == AfterIQ::Close || after_iq == AfterIQ::Reset)
            {
                close(listen_fd);
                listen_fd = -1;
            }
            client_left = true;
        }

    public:
        // With a port, that port is used instead of a free one, e.g. to bring a server back on
        // the port of one that left
        explicit FakeRtlTcp(const std::vector<uint8_t> &iq = {}, bool send_dongle_info = true, double samplerate = 0, AfterIQ after = AfterIQ::Silence, int port = 0)
            : bytes_per_second(samplerate * 2), after_iq(after)
        {
            if (send_dongle_info)
                data = dongleInfo();
            iq_start = data.size();
            data.insert(data.end(), iq.begin(), iq.end());

            listen_fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            int reuse = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            socklen_t len = sizeof(addr);
            if (bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
            {
                close(listen_fd);
                throw std::runtime_error("FakeRtlTcp: cannot bind port " + std::to_string(port) + ": " + strerror(errno));
            }
            listen(listen_fd, 1);
            getsockname(listen_fd, (sockaddr *)&addr, &len);
            listen_port = ntohs(addr.sin_port);

            th = std::thread([this]() { serve(); });
        }

        ~FakeRtlTcp()
        {
            should_stop = true;
            if (th.joinable())
                th.join();
            if (listen_fd >= 0)
                close(listen_fd);
        }

        FakeRtlTcp(const FakeRtlTcp &) = delete;
        FakeRtlTcp &operator=(const FakeRtlTcp &) = delete;

        int port() const { return listen_port; }

        // A client connected (it may have left since)
        bool connected() const { return client_connected; }

        // The connection was closed, by the client or by the server (see AfterIQ)
        bool disconnected() const { return client_left; }

        // Bytes of `iq` sent so far
        uint64_t iqSent() const { return sent; }

        // Commands received so far, in order
        std::vector<Command> commands()
        {
            std::lock_guard<std::mutex> l(mtx);
            return received;
        }

        // Waits (bounded) for at least `count` commands, and returns what was received
        std::vector<Command> waitForCommands(size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(10))
        {
            test_helpers::waitUntil([&]() { return commands().size() >= count; }, timeout, std::chrono::milliseconds(1));
            return commands();
        }

        // Waits (bounded) for the connection to be closed
        bool waitForDisconnect()
        {
            return test_helpers::waitUntil([&]() { return disconnected(); }, std::chrono::seconds(10), std::chrono::milliseconds(1));
        }
    };
} // namespace rtltcp_test
#endif
