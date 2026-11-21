#include "xrit_support/xrit/goes/segment_decoder.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace xrit_test;
namespace xr = satdump::xrit;

namespace
{
    // A segment of a GOES-R image, 16 x 4 pixels, every pixel set to the segment number. GOES-R
    // segments are numbered from 0
    xr::XRITFile goesRSegment(uint16_t segment, uint16_t max_segment, uint16_t max_row)
    {
        xr::XRITFile f;
        f.lrit_data =
            lritFile(0, {imageStructureRecord(8, 16, 4, 0), annotationRecord("GOES_SEGMENT.lrit"), noaaRecord(16, 13, 0, 0), segmentIdentificationRecord(1, segment, 0, 0, max_segment, 16, max_row)},
                     Bytes(16 * 4, segment));
        f.parseHeaders();
        return f;
    }
} // namespace

TEST_CASE("GOESSegmentedImageDecoder places a segment by its number", "[plugins][xrit_support][goes_segment_decoder]")
{
    auto first = goesRSegment(1, 2, 8);
    xr::GOESSegmentedImageDecoder decoder(first);
    decoder.pushSegment(first);

    REQUIRE(decoder.image.size() == 16 * 8);
    CHECK(decoder.hasData());
    CHECK_FALSE(decoder.isComplete());
    CHECK(decoder.image.get(16 * 4 - 1) == 0);
    CHECK(decoder.image.get(16 * 4) == 1);
}

TEST_CASE("GOESSegmentedImageDecoder ignores the segments of an image of 0 segments", "[plugins][xrit_support][goes_segment_decoder]")
{
    // The lines of a segment are max_row / max_segment. identifyXRITFIle does not give such a
    // segment a seg_groupid, so the decoder is not built for it in a decoding run
    uint16_t max_row = GENERATE(8, 0);
    auto file = goesRSegment(1, 0, max_row);
    xr::GOESSegmentedImageDecoder decoder(file);
    decoder.pushSegment(file);

    CHECK_FALSE(decoder.hasData());
}
