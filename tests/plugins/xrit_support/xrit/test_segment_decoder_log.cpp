#include "cli_test_helpers.h"
#include "xrit_support/xrit/gk2a/segment_decoder.h"
#include "xrit_support/xrit/himawari/segment_decoder.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>

using namespace xrit_test;
namespace xr = satdump::xrit;

// The "wrong size" errors of the GK-2A and Himawari segment decoders print image sizes, which
// are size_t, and used to pass them to %d

namespace
{
    // A file of 16 x 4 pixel segments
    xr::XRITFile segmentFile()
    {
        xr::XRITFile f;
        f.lrit_data = lritFile(0, {imageStructureRecord(8, 16, 4, 0)}, Bytes(16 * 4, 1));
        f.parseHeaders();
        return f;
    }
} // namespace

TEST_CASE("GK2ASegmentedImageDecoder logs the sizes of a segment of the wrong size", "[plugins][xrit_support][segment_decoder_log]")
{
    auto file = segmentFile();
    xr::GK2ASegmentedImageDecoder decoder(file);
    satdump::image::Image narrow(8, 8, 3, 1);

    cli_test::LogCapture log;
    decoder.pushSegment(narrow, 0, 2);
    INFO(log.dump());
    // The GK-2A image is segment width x segment width
    CHECK(log.contains("Image of the wrong size! 16x4 | 8x3 | 5/16", slog::LOG_ERROR));
    CHECK_FALSE(decoder.hasData());
}

TEST_CASE("HimawariSegmentedImageDecoder logs the sizes of a segment of the wrong size", "[plugins][xrit_support][segment_decoder_log]")
{
    auto file = segmentFile();
    xr::HimawariSegmentedImageDecoder decoder(file);
    decoder.info.channel = "B13";
    satdump::image::Image small(8, 8, 3, 1);

    cli_test::LogCapture log;
    decoder.pushSegment(small, 0);
    INFO(log.dump());
    CHECK(log.contains("Image of the wrong size! (B13) 16x4 Got 8x3", slog::LOG_ERROR));
    CHECK_FALSE(decoder.hasData());
}
