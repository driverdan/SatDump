#include "helpers/http_server.h"
#include "helpers/sockets.h"
#include "webhook_app/webhook.h"
#include <catch2/catch_test_macros.hpp>
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

TEST_CASE("send_webhook reports a webhook it cannot reach", "[webhook_app]")
{
    NoProxy np;
    CHECK_FALSE(webhook_app::WebhookSender::send_webhook("http://127.0.0.1:" + std::to_string(closedPort()) + "/hook", "text", "Hello"));
}
#endif
