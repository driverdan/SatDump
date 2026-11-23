#include "common/tracking/tle.h"
#include "core/config.h"
#include "helpers/http_server.h"
#include "helpers/satdump_guards.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // ISS (ZARYA) element set from 2008, the widely published example of the TLE format.
    // Both checksums (last digit) were verified by hand: digits summed, '-' counting as 1, mod 10.
    const std::string ISS_NAME = "ISS (ZARYA)";
    const std::string ISS_L1 = "1 25544U 98067A   08264.51782528 -.00002182  00000-0 -11606-4 0  2927";
    const std::string ISS_L2 = "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.72125391563537";

    // Vanguard 1, the first SGP4 verification case in Vallado et al. "Revisiting Spacetrack Report #3"
    const std::string VAN_NAME = "VANGUARD 1";
    const std::string VAN_L1 = "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753";
    const std::string VAN_L2 = "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667";

    int parse(const std::string &text, std::vector<satdump::TLE> &out)
    {
        std::istringstream in(text);
        return satdump::parseTLEStream(in, out);
    }
} // namespace

TEST_CASE("parseTLEStream parses a single named TLE", "[common][tracking][tle]")
{
    std::vector<satdump::TLE> tles;
    CHECK(parse(ISS_NAME + "\n" + ISS_L1 + "\n" + ISS_L2 + "\n", tles) == 1);
    REQUIRE(tles.size() == 1);
    CHECK(tles[0].norad == 25544);
    CHECK(tles[0].name == ISS_NAME);
    CHECK(tles[0].line1 == ISS_L1);
    CHECK(tles[0].line2 == ISS_L2);
}

TEST_CASE("parseTLEStream parses zero-padded catalog numbers", "[common][tracking][tle]")
{
    std::vector<satdump::TLE> tles;
    CHECK(parse(VAN_NAME + "\n" + VAN_L1 + "\n" + VAN_L2, tles) == 1);
    REQUIRE(tles.size() == 1);
    CHECK(tles[0].norad == 5);
    CHECK(tles[0].name == VAN_NAME);
}

TEST_CASE("parseTLEStream trims whitespace, CRLF and blank lines", "[common][tracking][tle]")
{
    std::vector<satdump::TLE> tles;
    std::string text = "\r\n   " + ISS_NAME + "  \r\n\n\t" + ISS_L1 + "\t\r\n\r\n" + ISS_L2 + "   \r\n";
    CHECK(parse(text, tles) == 1);
    REQUIRE(tles.size() == 1);
    CHECK(tles[0].name == ISS_NAME);
    CHECK(tles[0].line1 == ISS_L1);
    CHECK(tles[0].line2 == ISS_L2);
}

TEST_CASE("parseTLEStream rejects bad checksums on either line", "[common][tracking][tle]")
{
    std::vector<satdump::TLE> tles;

    std::string bad1 = ISS_L1;
    bad1.back() = '8';
    CHECK(parse(ISS_NAME + "\n" + bad1 + "\n" + ISS_L2, tles) == 0);

    std::string bad2 = ISS_L2;
    bad2.back() = '0';
    CHECK(parse(ISS_NAME + "\n" + ISS_L1 + "\n" + bad2, tles) == 0);

    // A '+' counts as 0 while '-' counts as 1, so swapping one changes the checksum
    std::string plus = ISS_L1;
    plus[33] = '+';
    REQUIRE(ISS_L1[33] == '-');
    CHECK(parse(ISS_NAME + "\n" + plus + "\n" + ISS_L2, tles) == 0);

    // A corrupted digit
    std::string digit = ISS_L2;
    digit[10] = '2';
    REQUIRE(ISS_L2[10] == '1');
    CHECK(parse(ISS_NAME + "\n" + ISS_L1 + "\n" + digit, tles) == 0);

    // Non-digit checksum character
    std::string letter = ISS_L1;
    letter.back() = 'X';
    CHECK(parse(ISS_NAME + "\n" + letter + "\n" + ISS_L2, tles) == 0);

    CHECK(tles.empty());
}

TEST_CASE("parseTLEStream rejects lines in the wrong order", "[common][tracking][tle]")
{
    std::vector<satdump::TLE> tles;
    CHECK(parse(ISS_NAME + "\n" + ISS_L2 + "\n" + ISS_L1, tles) == 0);
    CHECK(tles.empty());
}

TEST_CASE("parseTLEStream resynchronises after garbage lines", "[common][tracking][tle]")
{
    std::vector<satdump::TLE> tles;
    std::string text = "some header\nanother line\n" + ISS_NAME + "\n" + ISS_L1 + "\n" + ISS_L2 + "\nnot a tle\n" + VAN_NAME + "\n" + VAN_L1 + "\n" + VAN_L2 + "\n";
    CHECK(parse(text, tles) == 2);
    REQUIRE(tles.size() == 2);
    // Output is sorted by name
    CHECK(tles[0].name == ISS_NAME);
    CHECK(tles[1].name == VAN_NAME);
    CHECK(tles[1].norad == 5);
}

TEST_CASE("parseTLEStream skips non-numeric catalog numbers", "[common][tracking][tle]")
{
    // Alpha-5 style "X5544": the checksum is adjusted so only the catalog number parse fails
    std::string l2 = ISS_L2;
    l2[2] = 'X';
    l2.back() = '5';
    std::vector<satdump::TLE> tles;
    CHECK(parse("ALPHA5\n" + ISS_L1 + "\n" + l2, tles) == 0);
    CHECK(tles.empty());
}

TEST_CASE("parseTLEStream sorts by name and removes adjacent NORAD duplicates", "[common][tracking][tle]")
{
    std::vector<satdump::TLE> tles;
    std::string text = VAN_NAME + "\n" + VAN_L1 + "\n" + VAN_L2 + "\n" + //
                       ISS_NAME + "\n" + ISS_L1 + "\n" + ISS_L2 + "\n" + //
                       ISS_NAME + "\n" + ISS_L1 + "\n" + ISS_L2 + "\n";
    // The return value counts every parsed set, before de-duplication
    CHECK(parse(text, tles) == 3);
    REQUIRE(tles.size() == 2);
    CHECK(tles[0].norad == 25544);
    CHECK(tles[1].norad == 5);
}

TEST_CASE("parseTLEStream appends to and de-duplicates an existing registry", "[common][tracking][tle]")
{
    std::vector<satdump::TLE> tles;
    tles.push_back({25544, ISS_NAME, ISS_L1, ISS_L2, 0});
    CHECK(parse(VAN_NAME + "\n" + VAN_L1 + "\n" + VAN_L2 + "\n" + ISS_NAME + "\n" + ISS_L1 + "\n" + ISS_L2, tles) == 2);
    REQUIRE(tles.size() == 2);
    CHECK(tles[0].name == ISS_NAME);
    CHECK(tles[1].name == VAN_NAME);
}

TEST_CASE("parseTLEStream on empty input returns nothing", "[common][tracking][tle]")
{
    std::vector<satdump::TLE> tles;
    CHECK(parse("", tles) == 0);
    CHECK(parse("\n\n   \n", tles) == 0);
    CHECK(parse(ISS_L1 + "\n" + ISS_L2, tles) == 0); // No name line
    CHECK(tles.empty());
}

TEST_CASE("get_from_norad_in_vec finds TLEs by catalog number", "[common][tracking][tle]")
{
    std::vector<satdump::TLE> tles = {{25544, ISS_NAME, ISS_L1, ISS_L2, 0}, {5, VAN_NAME, VAN_L1, VAN_L2, 0}};
    auto found = satdump::get_from_norad_in_vec(tles, 5);
    REQUIRE(found.has_value());
    CHECK(found->name == VAN_NAME);
    CHECK(found->line2 == VAN_L2);
    CHECK_FALSE(satdump::get_from_norad_in_vec(tles, 12345).has_value());
    CHECK_FALSE(satdump::get_from_norad_in_vec({}, 5).has_value());
}

TEST_CASE("TLE JSON serialisation round-trips the element set", "[common][tracking][tle]")
{
    satdump::TLE t{25544, ISS_NAME, ISS_L1, ISS_L2, 1234};
    nlohmann::json j = t;
    CHECK(j["norad"] == 25544);
    CHECK(j["name"] == ISS_NAME);
    CHECK(j["line1"] == ISS_L1);
    CHECK(j["line2"] == ISS_L2);
    // The time field is not serialised
    CHECK_FALSE(j.contains("time"));

    satdump::TLE back = j.get<satdump::TLE>();
    CHECK(back.norad == t.norad);
    CHECK(back.name == t.name);
    CHECK(back.line1 == t.line1);
    CHECK(back.line2 == t.line2);
}

#ifndef _WIN32
// The downloads only ever go to a loopback server started by the test (helpers/http_server.h)
TEST_CASE("tryFetchTLEsFromFileURL does not retry a refused request", "[common][tracking][tle]")
{
    // #232: a 403 / 404 used to be requested 10 times. The second response is only there to record a retry.
    test_helpers::NoProxy np;
    std::string status = GENERATE(as<std::string>{}, "403 Forbidden", "404 Not Found");
    test_helpers::LoopbackHttpServer server({{status, ISS_NAME + "\n" + ISS_L1 + "\n" + ISS_L2 + "\n"}, {status, ""}});
    CHECK(satdump::tryFetchTLEsFromFileURL(server.url("/tles.txt")).empty());
    server.stop();
    CHECK(server.requests.size() == 1);
}

TEST_CASE("tryFetchTLEsFromFileURL retries a server error and only parses the last response", "[common][tracking][tle]")
{
    // The body of the failed attempt must not be parsed along with the next one
    test_helpers::NoProxy np;
    test_helpers::LoopbackHttpServer server({{"503 Service Unavailable", ISS_NAME + "\n" + ISS_L1 + "\n" + ISS_L2 + "\n"}, {"200 OK", VAN_NAME + "\n" + VAN_L1 + "\n" + VAN_L2 + "\n"}});
    auto tles = satdump::tryFetchTLEsFromFileURL(server.url("/tles.txt"));
    server.finish();
    CHECK(server.requests.size() == 2);
    REQUIRE(tles.size() == 1);
    CHECK(tles[0].norad == 5);
    CHECK(tles[0].name == VAN_NAME);
}

TEST_CASE("tryFetchSingleTLEwithNorad does not retry a refused request", "[common][tracking][tle]")
{
    test_helpers::ConfigGuard cfg;
    test_helpers::NoProxy np;
    test_helpers::LoopbackHttpServer server({{"404 Not Found", "No GP data found"}, {"404 Not Found", ""}});
    satdump::satdump_cfg.main_cfg["tle_settings"]["url_template"] = server.url("/gp.php?CATNR=%NORAD%");
    CHECK(satdump::tryFetchSingleTLEwithNorad(25544).empty());
    server.stop();
    REQUIRE(server.requests.size() == 1);
    CHECK(server.requests[0].rfind("GET /gp.php?CATNR=25544 HTTP/1.1\r\n", 0) == 0);
}
#endif
