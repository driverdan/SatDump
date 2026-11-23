#pragma once

/*
Loopback HTTP server for the tests of code that downloads, shared by the whole
suite. Requests only ever go to 127.0.0.1, to a server started by the test
itself. POSIX sockets (and setenv/unsetenv) only, so the whole header is empty
on Windows: wrap its users in #ifndef _WIN32 as well.
*/

#ifndef _WIN32
#include <arpa/inet.h>
#include <atomic>
#include <cstdlib>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace test_helpers
{
    // Sets or unsets an environment variable, restoring the previous value afterwards
    struct EnvGuard
    {
        std::string name;
        std::optional<std::string> old;
        EnvGuard(const std::string &n, const char *value) : name(n)
        {
            const char *o = getenv(n.c_str());
            if (o)
                old = o;
            if (value)
                setenv(n.c_str(), value, 1);
            else
                unsetenv(n.c_str());
        }
        ~EnvGuard()
        {
            if (old)
                setenv(name.c_str(), old->c_str(), 1);
            else
                unsetenv(name.c_str());
        }
    };

    // Make sure no proxy configured in the environment intercepts loopback requests
    struct NoProxy
    {
        EnvGuard a{"http_proxy", nullptr}, b{"HTTP_PROXY", nullptr}, c{"all_proxy", nullptr}, d{"ALL_PROXY", nullptr};
        EnvGuard e{"no_proxy", "*"}, f{"NO_PROXY", "*"};
    };

    struct HttpResponse
    {
        std::string status; // e.g. "200 OK"
        std::string body;
        std::string headers = ""; // extra response header lines, each ending with \r\n
    };

    // Answers one connection on 127.0.0.1 per given response, in order, recording each request.
    // It then stops listening, so any further request is refused instead of hanging.
    // Every blocking step is bounded by poll() so a failed request cannot hang the test.
    // To check that code does NOT make another request, give it a spare response, then call
    // stop() and look at requests: a request made anyway is answered and recorded.
    class LoopbackHttpServer
    {
    private:
        int listen_fd = -1;
        std::thread th;
        std::vector<std::string> responses;
        std::atomic<bool> stopping{false};

        static bool waitReadable(int fd)
        {
            pollfd p{fd, POLLIN, 0};
            return poll(&p, 1, 15000) > 0;
        }

        std::string serveOne(int c, const std::string &response)
        {
            std::string request;
            size_t header_end = std::string::npos;
            size_t content_length = 0;
            char buf[4096];
            while (true)
            {
                if (header_end != std::string::npos && request.size() >= header_end + 4 + content_length)
                    break;
                if (!waitReadable(c))
                    break;
                ssize_t n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0)
                    break;
                request.append(buf, n);
                if (header_end == std::string::npos && (header_end = request.find("\r\n\r\n")) != std::string::npos)
                {
                    size_t cl = request.find("Content-Length: ");
                    if (cl != std::string::npos && cl < header_end)
                        content_length = std::stoul(request.substr(cl + 16));
                }
            }

            size_t sent = 0;
            while (sent < response.size())
            {
                ssize_t n = send(c, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                if (n <= 0)
                    break;
                sent += n;
            }
            return request;
        }

        // Waits up to 15 s for a connection, giving up early once stop() is called
        bool waitConnection()
        {
            for (int i = 0; i < 150 && !stopping; i++)
            {
                pollfd p{listen_fd, POLLIN, 0};
                if (poll(&p, 1, 100) > 0)
                    return true;
            }
            return false;
        }

        void serve()
        {
            for (auto &response : responses)
            {
                if (!waitConnection())
                    break;
                int c = accept(listen_fd, nullptr, nullptr);
                if (c < 0)
                    break;
                requests.push_back(serveOne(c, response));
                close(c);
            }
            close(listen_fd);
            listen_fd = -1;
        }

    public:
        int port = 0;
        std::vector<std::string> requests;

        explicit LoopbackHttpServer(const std::vector<HttpResponse> &rsps)
        {
            for (auto &r : rsps)
                responses.push_back("HTTP/1.1 " + r.status + "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\n" + r.headers +
                                    "Connection: close\r\n\r\n" + r.body);

            listen_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd < 0)
                throw std::runtime_error("socket() failed");
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            socklen_t len = sizeof(addr);
            if (bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 1) != 0 || getsockname(listen_fd, (sockaddr *)&addr, &len) != 0)
            {
                close(listen_fd);
                throw std::runtime_error("Could not set up loopback server");
            }
            port = ntohs(addr.sin_port);
            th = std::thread(&LoopbackHttpServer::serve, this);
        }

        // Waits for every exchange to finish (or time out), so requests is complete and safe to read
        void finish()
        {
            if (th.joinable())
                th.join();
        }

        // Stops waiting for the connections still expected, so requests is complete and safe to read
        void stop()
        {
            stopping = true;
            finish();
        }

        ~LoopbackHttpServer() { stop(); }

        std::string url(const std::string &path) { return "http://127.0.0.1:" + std::to_string(port) + path; }
    };

    // Answers a single request
    class OneShotHttpServer : public LoopbackHttpServer
    {
    public:
        std::string request;

        OneShotHttpServer(const std::string &status, const std::string &body, const std::string &headers = "") : LoopbackHttpServer({{status, body, headers}}) {}

        void finish()
        {
            LoopbackHttpServer::finish();
            if (!requests.empty())
                request = requests[0];
        }
    };
} // namespace test_helpers
#endif
