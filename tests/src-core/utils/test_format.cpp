#include "utils/format.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <string>

using namespace satdump;

TEST_CASE("to_string_with_precision uses fixed notation", "[utils][format]")
{
    CHECK(to_string_with_precision(3.14159, 2) == "3.14");
    CHECK(to_string_with_precision(2.5, 0) == "2");
    CHECK(to_string_with_precision(1.0, 3) == "1.000");
    CHECK(to_string_with_precision(1.5f) == "1.500000");
    CHECK(to_string_with_precision(-0.126, 2) == "-0.13");
}

TEST_CASE("svformat behaves like printf", "[utils][format]")
{
    CHECK(svformat("plain") == "plain");
    CHECK(svformat("%d-%s", 5, "a") == "5-a");
    CHECK(svformat("%05.1f MHz", 137.1) == "137.1 MHz");
    CHECK(svformat("%02X%02X", 0xAB, 0x01) == "AB01");
}

TEST_CASE("svformat handles output longer than its initial buffer", "[utils][format]")
{
    std::string big(2000, 'x');
    std::string formatted = svformat("%s", big.c_str());

    CHECK(formatted.size() == big.size());
    CHECK((formatted == big));
}

// The initial buffer is 1024 bytes, so 1023 characters plus the NUL is the most
// that fits on the first try. 1024 and up must take the resize path.
TEST_CASE("svformat keeps every character around the initial buffer size", "[utils][format]")
{
    for (size_t len : {1022, 1023, 1024, 1025, 2047, 2048})
    {
        CAPTURE(len);
        std::string s(len - 1, 'x');
        s += 'y'; // distinct last character, so a dropped one is visible
        std::string formatted = svformat("%s", s.c_str());

        CHECK(formatted.size() == len);
        CHECK((formatted == s));
    }
}

// In the "C" locale a wide character above 0xFF cannot be converted by %ls, so
// snprintf fails with EILSEQ and returns a negative value.
TEST_CASE("svformat returns an empty string when snprintf fails", "[utils][format]")
{
    const wchar_t bad[] = {(wchar_t)0x1234, 0};
    if (snprintf(nullptr, 0, "%ls", bad) >= 0)
        SKIP("this C library does not fail on an unconvertible %ls argument");

    std::string formatted;
    CHECK_NOTHROW(formatted = svformat("%ls", bad));
    CHECK(formatted.empty());
}
