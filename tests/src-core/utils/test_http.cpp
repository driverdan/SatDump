#include "helpers/http_server.h"
#include "helpers/sockets.h"
#include "helpers/temp_files.h"
#include "satdump_vars.h"
#include "utils/http.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <chrono>
#include <curl/curl.h>
#include <string>

// Not declared in http.h, but exported by satdump_core
namespace satdump
{
    int curl_float_progress_func(void *ptr, curl_off_t TotalToDownload, curl_off_t NowDownloaded, curl_off_t TotalToUpload, curl_off_t NowUploaded);
}

// Requests only ever go to a loopback HTTP server started by the test itself (helpers/http_server.h).
// That server uses POSIX sockets, so the tests needing it are POSIX only.

#ifndef _WIN32
namespace
{
    using test_helpers::closedPort;
    using test_helpers::LoopbackHttpServer;
    using test_helpers::NoProxy;
    using test_helpers::OneShotHttpServer;

    bool contains(const std::string &haystack, const std::string &needle) { return haystack.find(needle) != std::string::npos; }
} // namespace
#endif

TEST_CASE("curl_write_std_string appends the chunk and reports its size", "[utils][http]")
{
    std::string s = "ab";
    char data[] = "cdefgh";
    CHECK(satdump::curl_write_std_string(data, 2, 3, &s) == 6);
    CHECK(s == "abcdefgh");
    CHECK(satdump::curl_write_std_string(data, 1, 0, &s) == 0);
    CHECK(s == "abcdefgh");
}

TEST_CASE("curl_float_progress_func reports download, then upload, progress", "[utils][http]")
{
    float p = -1;
    CHECK(satdump::curl_float_progress_func(&p, 200, 50, 0, 0) == 0);
    CHECK(p == Catch::Approx(0.25f));

    // Download takes precedence when both are known
    CHECK(satdump::curl_float_progress_func(&p, 10, 10, 100, 1) == 0);
    CHECK(p == Catch::Approx(1.0f));

    CHECK(satdump::curl_float_progress_func(&p, 0, 0, 40, 30) == 0);
    CHECK(p == Catch::Approx(0.75f));

    // Unknown totals leave the value untouched
    p = 0.5f;
    CHECK(satdump::curl_float_progress_func(&p, 0, 0, 0, 0) == 0);
    CHECK(p == 0.5f);
}

#ifndef _WIN32
TEST_CASE("perform_http_request fetches a body over loopback", "[utils][http]")
{
    NoProxy np;
    OneShotHttpServer server("200 OK", "hello from the test server");
    std::string result;
    float progress = 0;
    int ret = satdump::perform_http_request(server.url("/some/path?x=1"), result, "X-Unit-Test: yes", &progress);
    server.finish();

    CHECK(ret == 0);
    CHECK(result == "hello from the test server");
    CHECK(progress > 0.0f);
    CHECK(progress <= 1.0f);

    CHECK(server.request.rfind("GET /some/path?x=1 HTTP/1.1\r\n", 0) == 0);
    CHECK(contains(server.request, "\r\nUser-Agent: SatDump/v" + satdump::SATDUMP_VERSION + "\r\n"));
    CHECK(contains(server.request, "\r\nX-Unit-Test: yes\r\n"));
}

TEST_CASE("perform_http_request appends to an existing result string", "[utils][http]")
{
    // The result is not cleared first, so callers reusing a string accumulate responses
    NoProxy np;
    OneShotHttpServer server("200 OK", "body");
    std::string result = "previous;";
    CHECK(satdump::perform_http_request(server.url("/"), result) == 0);
    CHECK(result == "previous;body");
}

TEST_CASE("perform_http_request fails on HTTP error statuses", "[utils][http]")
{
    // The error page is still appended to the result
    NoProxy np;
    std::string status = GENERATE(as<std::string>{}, "404 Not Found", "500 Internal Server Error");
    OneShotHttpServer server(status, "nothing here");
    std::string result;
    CHECK(satdump::perform_http_request(server.url("/missing"), result) == 1);
    CHECK(result == "nothing here");
}

TEST_CASE("perform_http_request reports the final HTTP status", "[utils][http]")
{
    NoProxy np;
    long status = -1;
    std::string result;

    SECTION("on success")
    {
        OneShotHttpServer server("200 OK", "body");
        CHECK(satdump::perform_http_request(server.url("/"), result, "", nullptr, &status) == 0);
        CHECK(status == 200);
    }

    SECTION("on an HTTP error")
    {
        OneShotHttpServer server("403 Forbidden", "go away");
        CHECK(satdump::perform_http_request(server.url("/"), result, "", nullptr, &status) == 1);
        CHECK(status == 403);
    }

    SECTION("after following a redirect")
    {
        OneShotHttpServer target("503 Service Unavailable", "later");
        OneShotHttpServer redirect("301 Moved Permanently", "", "Location: " + target.url("/") + "\r\n");
        CHECK(satdump::perform_http_request(redirect.url("/"), result, "", nullptr, &status) == 1);
        CHECK(status == 503);
    }

    SECTION("as 0 when no response was received")
    {
        CHECK(satdump::perform_http_request("http://127.0.0.1:" + std::to_string(closedPort()) + "/", result, "", nullptr, &status) == 1);
        CHECK(status == 0);
    }
}

TEST_CASE("perform_http_request answers several requests from the same loopback server", "[utils][http]")
{
    // Checks the helper the retry tests of the TLE / Kepler downloads rely on
    NoProxy np;
    LoopbackHttpServer server({{"500 Internal Server Error", "first"}, {"200 OK", "second"}});
    std::string a, b, c;
    CHECK(satdump::perform_http_request(server.url("/1"), a) == 1);
    CHECK(satdump::perform_http_request(server.url("/2"), b) == 0);
    server.finish();
    CHECK(a == "first");
    CHECK(b == "second");
    REQUIRE(server.requests.size() == 2);
    CHECK(server.requests[1].rfind("GET /2 HTTP/1.1\r\n", 0) == 0);

    // Once every response is used, further requests are refused rather than left hanging
    CHECK(satdump::perform_http_request(server.url("/3"), c) == 1);
}
#endif

TEST_CASE("http_request_should_retry only retries transient failures", "[utils][http]")
{
    // No response at all (transport error), rate limiting and server errors
    CHECK(satdump::http_request_should_retry(0));
    CHECK(satdump::http_request_should_retry(429));
    CHECK(satdump::http_request_should_retry(500));
    CHECK(satdump::http_request_should_retry(503));

    // A refusal or a missing document would only be answered the same way again
    CHECK_FALSE(satdump::http_request_should_retry(301));
    CHECK_FALSE(satdump::http_request_should_retry(400));
    CHECK_FALSE(satdump::http_request_should_retry(403));
    CHECK_FALSE(satdump::http_request_should_retry(404));
}

#ifndef _WIN32

TEST_CASE("perform_http_request follows redirects", "[utils][http]")
{
    // #75: the redirect page used to be returned as the document
    NoProxy np;
    OneShotHttpServer target("200 OK", "the document");
    OneShotHttpServer redirect("301 Moved Permanently", "<html>Object Moved</html>", "Location: " + target.url("/moved/file.txt") + "\r\n");
    std::string result;
    int ret = satdump::perform_http_request(redirect.url("/file.txt"), result, "X-Unit-Test: yes");
    redirect.finish();
    target.finish();

    CHECK(ret == 0);
    CHECK(result == "the document");
    CHECK(target.request.rfind("GET /moved/file.txt HTTP/1.1\r\n", 0) == 0);
}

TEST_CASE("perform_http_request fails on a redirect it cannot follow", "[utils][http]")
{
    NoProxy np;
    OneShotHttpServer server("302 Found", "no location given");
    std::string result;
    CHECK(satdump::perform_http_request(server.url("/"), result) == 1);
}

TEST_CASE("perform_http_request fails on a refused connection", "[utils][http]")
{
    NoProxy np;
    std::string result;
    CHECK(satdump::perform_http_request("http://127.0.0.1:" + std::to_string(closedPort()) + "/", result) == 1);
    CHECK(result.empty());
}

#endif

// file:// URLs are built from a POSIX path
#ifndef _WIN32
TEST_CASE("perform_http_request reads file URLs, which have no HTTP status", "[utils][http]")
{
    test_helpers::TempFile file("http_file_url.txt", "local content");
    std::string result;
    CHECK(satdump::perform_http_request("file://" + file.path.string(), result) == 0);
    CHECK(result == "local content");
}
#endif

TEST_CASE("perform_http_request fails on an unsupported URL scheme", "[utils][http]")
{
    std::string result;
    CHECK(satdump::perform_http_request("satdumpnoproto://127.0.0.1/x", result) == 1);
    CHECK(result.empty());
}

#ifndef _WIN32
TEST_CASE("perform_http_request_post sends the payload and headers", "[utils][http]")
{
    NoProxy np;
    OneShotHttpServer server("200 OK", "{\"ok\":true}");
    std::string result;
    int ret = satdump::perform_http_request_post(server.url("/api"), result, "a=1&b=two", "X-Post-Test: 42");
    server.finish();

    CHECK(ret == 0);
    CHECK(result == "{\"ok\":true}");
    CHECK(server.request.rfind("POST /api HTTP/1.1\r\n", 0) == 0);
    CHECK(contains(server.request, "\r\nUser-Agent: SatDump/v" + satdump::SATDUMP_VERSION + "\r\n"));
    CHECK(contains(server.request, "\r\nX-Post-Test: 42\r\n"));
    CHECK(contains(server.request, "\r\nContent-Length: 9\r\n"));
    CHECK(server.request.substr(server.request.size() - 9) == "a=1&b=two");
}

TEST_CASE("perform_http_request_post fails on a refused connection", "[utils][http]")
{
    NoProxy np;
    std::string result;
    CHECK(satdump::perform_http_request_post("http://127.0.0.1:" + std::to_string(closedPort()) + "/", result, "x=1") == 1);
    CHECK(result.empty());
}

TEST_CASE("perform_http_request_post fails on an HTTP error status", "[utils][http]")
{
    NoProxy np;
    auto status = GENERATE(as<std::string>{}, "301 Moved Permanently", "404 Not Found", "500 Internal Server Error");
    OneShotHttpServer server(status, "error page");
    std::string result;
    int ret = satdump::perform_http_request_post(server.url("/api"), result, "x=1");
    server.finish();

    CHECK(ret == 1);
    CHECK(result == "error page");
    CHECK(server.request.rfind("POST /api HTTP/1.1\r\n", 0) == 0);
}

// A loopback port that completes connections (in the backlog) but never answers them
class SilentServer
{
public:
    int fd = -1;
    int port = 0;

    SilentServer()
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        bool ok = bind(fd, (sockaddr *)&addr, sizeof(addr)) == 0 && listen(fd, 4) == 0 && getsockname(fd, (sockaddr *)&addr, &len) == 0;
        if (!ok)
            close(fd); // The destructor does not run when the constructor fails
        REQUIRE(ok);
        port = ntohs(addr.sin_port);
    }

    ~SilentServer() { close(fd); }

    std::string url() { return "http://127.0.0.1:" + std::to_string(port) + "/"; }
};

// Each of these waits for HTTP_STALL_TIMEOUT, so they are hidden: run them with "[slow]"
TEST_CASE("perform_http_request gives up on a server that never answers", "[.][slow][utils][http]")
{
    NoProxy np;
    SilentServer server;
    std::string result;
    long status = -1;
    auto start = std::chrono::steady_clock::now();
    CHECK(satdump::perform_http_request(server.url(), result, "", nullptr, &status) == 1);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();

    CHECK(status == 0);
    CHECK(elapsed >= satdump::HTTP_STALL_TIMEOUT - 1);
    CHECK(elapsed <= satdump::HTTP_STALL_TIMEOUT + 10);
}

TEST_CASE("perform_http_request_post gives up on a server that never answers", "[.][slow][utils][http]")
{
    NoProxy np;
    SilentServer server;
    std::string result;
    auto start = std::chrono::steady_clock::now();
    CHECK(satdump::perform_http_request_post(server.url(), result, "x=1") == 1);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();

    CHECK(elapsed >= std::min(satdump::HTTP_STALL_TIMEOUT, satdump::HTTP_POST_TIMEOUT) - 1);
    CHECK(elapsed <= satdump::HTTP_POST_TIMEOUT + 10);
}
#endif
