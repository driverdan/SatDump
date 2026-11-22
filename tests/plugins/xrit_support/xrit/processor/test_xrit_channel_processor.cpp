#include "cli_test_helpers.h"
#include "xrit_support/xrit/identify.h"
#include "xrit_support/xrit/processor/xrit_channel_processor.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>

using namespace xrit_test;
namespace xr = satdump::xrit;

namespace
{
    const std::string MSG_FILENAME = "H-000-MSG4__-MSG4________-VIS006___-000008___-202611210000-__";

    // A segment of a MSG image of 8 bits, channel 1
    xr::XRITFile msgSegment(uint16_t columns, uint16_t lines, uint16_t segment, uint16_t planned_start, uint16_t planned_end)
    {
        xr::XRITFile f;
        f.lrit_data =
            lritFile(0, {imageStructureRecord(8, columns, lines, 0), annotationRecord(MSG_FILENAME), msgSegmentIdentificationRecord(1, segment, planned_start, planned_end)}, Bytes(16 * 4, 7));
        f.parseHeaders();
        return f;
    }

    size_t imageCount(xr::XRITChannelProcessor &processor)
    {
        size_t n = 0;
        for (auto &p : processor.in_memory_products)
            n += p.second->images.size();
        return n;
    }
} // namespace

TEST_CASE("XRITChannelProcessor drops a segment of an image too large to allocate, and decodes the next image", "[plugins][xrit_support][xrit_channel_processor]")
{
    // 65535 x 30000 pixels by 65536 segments. The allocation used to throw out of push()
    xr::XRITChannelProcessor processor;
    processor.in_memory_mode = true;

    cli_test::LogCapture log;
    auto absurd = msgSegment(65535, 30000, 0, 0, 65535);
    auto absurd_info = xr::identifyXRITFIle(absurd);
    REQUIRE(absurd_info.seg_groupid != "");
    REQUIRE_NOTHROW(processor.push(absurd_info, absurd));
    CHECK(log.contains("File " + MSG_FILENAME + " gives an image of 65535x1966080000 pixels, more than the 1073741824 pixels a segmented image may have, its segments are ignored", slog::LOG_WARN));
    CHECK(imageCount(processor) == 0);

    // An image of a single segment, on the same channel
    auto segment = msgSegment(16, 4, 1, 1, 1);
    auto info = xr::identifyXRITFIle(segment);
    REQUIRE_NOTHROW(processor.push(info, segment));
    INFO(log.dump());
    REQUIRE(imageCount(processor) == 1);
    auto &image = processor.in_memory_products.begin()->second->images[0];
    CHECK(image.channel_name == "1");
    CHECK(image.image.width() == 16);
    CHECK(image.image.height() == 4);
    CHECK(image.image.get(0) == 7);
}
