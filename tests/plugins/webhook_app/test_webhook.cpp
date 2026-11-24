#include "helpers/http_server.h"
#include "helpers/sockets.h"
#include "logger.h"
#include "webhook_app/webhook.h"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <mutex>
#include <string>

// Webhooks are only ever sent to a loopback HTTP server started by the test itself
// (helpers/http_server.h), which uses POSIX sockets.

#ifndef _WIN32
namespace
{
    using test_helpers::closedPort;
    using test_helpers::NoProxy;
    using test_helpers::OneShotHttpServer;

    bool ends_with(const std::string &s, const std::string &suffix) { return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0; }

    // Replaces the global logger with one that only records, restoring it afterwards
    class LogCapture
    {
    private:
        class Sink : public slog::LoggerSink
        {
        public:
            std::mutex mtx;
            std::string all;

            void receive(slog::LogMsg log)
            {
                std::lock_guard<std::mutex> l(mtx);
                all += log.str + "\n";
            }
        };

        std::shared_ptr<slog::Logger> saved;
        std::shared_ptr<Sink> sink = std::make_shared<Sink>();

    public:
        LogCapture() : saved(logger)
        {
            logger = std::make_shared<slog::Logger>();
            logger->add_sink(sink);
            std::lock_guard<std::mutex> l(sink->mtx);
            sink->all.clear(); // add_sink replays earlier messages
        }
        ~LogCapture() { logger = saved; }
        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        std::string text()
        {
            std::lock_guard<std::mutex> l(sink->mtx);
            return sink->all;
        }
    };
} // namespace

TEST_CASE("send_webhook POSTs the message as JSON", "[webhook_app]")
{
    NoProxy np;
    OneShotHttpServer server("204 No Content", "");
    bool sent = webhook_app::WebhookSender::send_webhook(server.url("/hook"), "content", "Hello");
    server.finish();

    CHECK(sent);
    CHECK(server.request.rfind("POST /hook HTTP/1.1\r\n", 0) == 0);
    CHECK(server.request.find("\r\nContent-Type: application/json\r\n") != std::string::npos);
    CHECK(ends_with(server.request, "{\"content\":\"Hello\"}"));
}

TEST_CASE("send_webhook reports a webhook the server rejects", "[webhook_app]")
{
    // e.g. Discord, for a webhook that was deleted
    NoProxy np;
    OneShotHttpServer server("404 Not Found", "{\"message\": \"Unknown Webhook\", \"code\": 10015}");
    bool sent = webhook_app::WebhookSender::send_webhook(server.url("/hook"), "content", "Hello");
    server.finish();

    CHECK_FALSE(sent);
    CHECK(server.request.rfind("POST /hook HTTP/1.1\r\n", 0) == 0);
}

TEST_CASE("send_webhook does not log the webhook token, even when the server echoes it", "[webhook_app]")
{
    const std::string token = "SECRETTOKEN123";
    NoProxy np;
    LogCapture logs;
    OneShotHttpServer server("401 Unauthorized", "{\"message\": \"Invalid token /api/webhooks/1/" + token + "\"}\r\n(E) injected line");
    bool sent = webhook_app::WebhookSender::send_webhook(server.url("/api/webhooks/1/" + token), "content", "Hello");
    server.finish();

    CHECK_FALSE(sent);
    std::string out = logs.text();
    INFO(out);
    CHECK(out.find("Failed to send webhook") != std::string::npos);
    CHECK(out.find("status 401") != std::string::npos);
    CHECK(out.find(token) == std::string::npos);
    CHECK(out.find("injected line") == std::string::npos);
}

TEST_CASE("send_webhook reports a webhook it cannot reach", "[webhook_app]")
{
    NoProxy np;
    CHECK_FALSE(webhook_app::WebhookSender::send_webhook("http://127.0.0.1:" + std::to_string(closedPort()) + "/hook", "text", "Hello"));
}
#endif
