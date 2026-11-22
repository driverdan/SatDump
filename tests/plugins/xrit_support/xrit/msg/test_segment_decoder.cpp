#include "cli_test_helpers.h"
#include "xrit_support/xrit/msg/segment_decoder.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace xrit_test;
namespace xr = satdump::xrit;

namespace
{
    // A segment of a MSG image, 16 x 4 pixels of 8 bits, every pixel set to the segment number
    xr::XRITFile msgSegment(uint16_t segment, uint16_t planned_start, uint16_t planned_end)
    {
        xr::XRITFile f;
        f.lrit_data =
            lritFile(0, {imageStructureRecord(8, 16, 4, 0), annotationRecord("MSG_SEGMENT.lrit"), msgSegmentIdentificationRecord(1, segment, planned_start, planned_end)}, Bytes(16 * 4, segment));
        f.parseHeaders();
        return f;
    }
} // namespace

TEST_CASE("MSGSegmentedImageDecoder places a segment by its number from the planned start segment", "[plugins][xrit_support][msg_segment_decoder]")
{
    auto file = msgSegment(3, 2, 3);
    xr::MSGSegmentedImageDecoder decoder(file);
    decoder.pushSegment(file);

    REQUIRE(decoder.image.size() == 16 * 8);
    CHECK(decoder.hasData());
    CHECK_FALSE(decoder.isComplete());
    CHECK(decoder.image.get(16 * 4 - 1) == 0);
    CHECK(decoder.image.get(16 * 4) == 3);
}

TEST_CASE("MSGSegmentedImageDecoder ignores a segment whose number is outside the planned segments", "[plugins][xrit_support][msg_segment_decoder]")
{
    // An image of segments 2 and 3
    uint16_t segment = GENERATE(0, 1, 4, 65535);
    auto file = msgSegment(segment, 2, 3);
    xr::MSGSegmentedImageDecoder decoder(file);
    decoder.pushSegment(file);

    REQUIRE(decoder.image.size() == 16 * 8);
    CHECK_FALSE(decoder.hasData());
    CHECK_FALSE(decoder.isComplete());
    CHECK(decoder.image.get(0) == 0);
    CHECK(decoder.image.get(16 * 8 - 1) == 0);
}

TEST_CASE("MSGSegmentedImageDecoder ignores the segments of an image whose planned end segment is before its start", "[plugins][xrit_support][msg_segment_decoder]")
{
    // The image has end - start + 1 segments. identifyXRITFIle does not give such a segment a
    // seg_groupid, so the decoder is not built for it in a decoding run
    cli_test::LogCapture log;
    uint16_t planned_end = GENERATE(2, 7);
    auto file = msgSegment(8, 8, planned_end);
    xr::MSGSegmentedImageDecoder decoder(file);
    decoder.pushSegment(file);

    CHECK(decoder.image.size() == 0);
    CHECK_FALSE(decoder.hasData());
    CHECK(log.contains("File MSG_SEGMENT.lrit has its planned end segment (" + std::to_string(planned_end) + ") before its planned start segment (8), its segments are ignored", slog::LOG_WARN));
}
