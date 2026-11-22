#include "cli_test_helpers.h"
#include "xrit_support/xrit/identify.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace xrit_test;
namespace xr = satdump::xrit;

namespace
{
    xr::XRITFile parsedFile(const Bytes &data)
    {
        xr::XRITFile f;
        f.lrit_data = data;
        f.parseHeaders();
        return f;
    }

    // Segment 2 of a GOES-15 full disk image of 400 lines, whose navigation is shifted by the
    // lines of the segments before it
    xr::XRITFile goesNSegment(uint16_t max_segment)
    {
        return parsedFile(lritFile(0,
                                   {imageStructureRecord(8, 16, 4, 0), annotationRecord("GOES15_SEGMENT.lrit"), timeStampRecord(24228, 0), noaaRecord(15, 1, 0, 0),
                                    imageNavigationRecord("GEOS(-75.0)", 1, 1, 50, 100), segmentIdentificationRecord(1, 2, 0, 200, max_segment, 16, 400)},
                                   Bytes(64, 1)));
    }
} // namespace

TEST_CASE("identifyXRITFIle shifts the navigation of a GOES-N segment by the segments before it", "[plugins][xrit_support][identify]")
{
    auto file = goesNSegment(4);
    auto info = xr::identifyXRITFIle(file);

    CHECK(info.type == xr::XRIT_GOESN_IMAGER);
    CHECK(info.seg_groupid != "");
    REQUIRE(info.image_navigation_record);
    CHECK(info.image_navigation_record->line_offset == 100 + 2 * (400 / 4));
}

TEST_CASE("identifyXRITFIle keeps a GOES segment of an image of 0 segments as an image of its own", "[plugins][xrit_support][identify]")
{
    // The lines of a segment are max_row / max_segment
    cli_test::LogCapture log;
    auto file = goesNSegment(0);
    auto info = xr::identifyXRITFIle(file);

    CHECK(info.type == xr::XRIT_GOESN_IMAGER);
    CHECK(info.channel == "4");
    CHECK(info.seg_groupid == "");
    REQUIRE(info.image_navigation_record);
    CHECK(info.image_navigation_record->line_offset == 100);
    CHECK(log.contains("File GOES15_SEGMENT.lrit has 0 segments in its segment identification header", slog::LOG_WARN));
}

namespace
{
    const std::string MSG_FILENAME = "H-000-MSG4__-MSG4________-VIS006___-000008___-202611210000-__";
    const std::string ELEKTRO_FILENAME = "L-000-GOMS4_-GOMS4_4_____-00_9_0___-000008___-202611210000-__";
} // namespace

TEST_CASE("identifyXRITFIle groups the segments of a MSG or Elektro image", "[plugins][xrit_support][identify]")
{
    auto [filename, type] = GENERATE(std::make_pair(MSG_FILENAME, xr::XRIT_MSG_SEVIRI), std::make_pair(ELEKTRO_FILENAME, xr::XRIT_ELEKTRO_MSUGS));
    uint16_t planned_end = GENERATE(8, 10); // A single segment, or several
    auto file = parsedFile(lritFile(0, {imageStructureRecord(8, 16, 4, 0), annotationRecord(filename), msgSegmentIdentificationRecord(1, 8, 8, planned_end)}, Bytes(64, 1)));
    auto info = xr::identifyXRITFIle(file);

    CHECK(info.type == type);
    CHECK(info.seg_groupid != "");
}

TEST_CASE("identifyXRITFIle keeps a MSG or Elektro segment whose planned end segment is before its start as an image of its own", "[plugins][xrit_support][identify]")
{
    // The image would have end - start + 1 segments, 0 or less
    cli_test::LogCapture log;
    auto [filename, type] = GENERATE(std::make_pair(MSG_FILENAME, xr::XRIT_MSG_SEVIRI), std::make_pair(ELEKTRO_FILENAME, xr::XRIT_ELEKTRO_MSUGS));
    uint16_t planned_end = GENERATE(2, 7);
    auto file = parsedFile(lritFile(0, {imageStructureRecord(8, 16, 4, 0), annotationRecord(filename), msgSegmentIdentificationRecord(1, 8, 8, planned_end)}, Bytes(64, 1)));
    auto info = xr::identifyXRITFIle(file);

    CHECK(info.type == type);
    CHECK(info.channel != "");
    CHECK(info.seg_groupid == "");
    CHECK(
        log.contains("File " + filename + " has its planned end segment (" + std::to_string(planned_end) + ") before its planned start segment (8), saving it as an image of its own", slog::LOG_WARN));
}
