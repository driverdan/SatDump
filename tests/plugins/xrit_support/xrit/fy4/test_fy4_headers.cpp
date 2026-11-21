#include "xrit_support/xrit/fy4/fy4_headers.h"
#include "xrit_support/xrit/xrit_file.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>

using namespace xrit_test;
namespace xr = satdump::xrit;

namespace
{
    uint32_t floatBits(float f)
    {
        uint32_t v;
        std::memcpy(&v, &f, sizeof(v));
        return v;
    }

    // Type 2 as FY-4 sends it: the projection name padded to 32 characters, then CFAC, LFAC,
    // COFF and LOFF as big-endian 32 bit words
    Bytes fy4NavigationRecord(std::string projection, uint32_t column_scaling, uint32_t line_scaling, uint32_t column_offset, uint32_t line_offset)
    {
        projection.resize(32, ' ');
        Bytes c(projection.begin(), projection.end());
        put32(c, column_scaling);
        put32(c, line_scaling);
        put32(c, column_offset);
        put32(c, line_offset);
        return record(2, c);
    }
} // namespace

TEST_CASE("fy4::ImageNavigationRecord decodes its fields as big-endian floats", "[plugins][xrit_support][fy4_headers]")
{
    // Bit patterns written out, so the expected values do not depend on how the test encodes them
    Bytes rec = fy4NavigationRecord("GEOS(104.7)", 0x4B1C2531 /* 10233137 */, 0x3F800000 /* 1 */, 0x44ABB000 /* 1373.5 */, 0xC0490FDB /* -pi as a float */);
    REQUIRE(rec.size() == 51);

    xr::fy4::ImageNavigationRecord nav(rec.data());
    CHECK(nav.type == 2);
    CHECK(nav.record_length == 51);
    CHECK(nav.projection_name.substr(0, 11) == "GEOS(104.7)");
    CHECK(nav.projection_name.size() == 32);
    CHECK(nav.column_scaling_factor == 10233137.0f);
    CHECK(nav.line_scaling_factor == 1.0f);
    CHECK(nav.column_offset == 1373.5f);
    CHECK(floatBits(nav.line_offset) == 0xC0490FDB);
    CHECK(nav.column_scalar == 0.0); // Not in the record
    CHECK(nav.line_scalar == 0.0);
}

TEST_CASE("fy4::ImageNavigationRecord keeps each field apart from its neighbours", "[plugins][xrit_support][fy4_headers]")
{
    // Every byte of the four fields is different, so a field built from the wrong bytes shows
    Bytes rec = fy4NavigationRecord("GEOS(133.0)", 0x41424344, 0x45464748, 0xC1C2C3C4, 0xC5C6C7C8);

    xr::fy4::ImageNavigationRecord nav(rec.data());
    CHECK(floatBits(nav.column_scaling_factor) == 0x41424344);
    CHECK(floatBits(nav.line_scaling_factor) == 0x45464748);
    CHECK(floatBits(nav.column_offset) == 0xC1C2C3C4);
    CHECK(floatBits(nav.line_offset) == 0xC5C6C7C8);
}

TEST_CASE("fy4::ImageNavigationRecord is read from a file by XRITFile::getHeader", "[plugins][xrit_support][fy4_headers]")
{
    xr::XRITFile f;
    f.lrit_data = lritFile(0, {fy4NavigationRecord("GEOS(104.7)", floatBits(2047.75f), floatBits(-20466274.0f), floatBits(-0.5f), floatBits(10991.5f))}, {});
    f.parseHeaders();
    REQUIRE(f.hasHeader<xr::fy4::ImageNavigationRecord>());

    auto nav = f.getHeader<xr::fy4::ImageNavigationRecord>();
    CHECK(nav.column_scaling_factor == 2047.75f);
    CHECK(nav.line_scaling_factor == -20466274.0f);
    CHECK(nav.column_offset == -0.5f);
    CHECK(nav.line_offset == 10991.5f);
}
