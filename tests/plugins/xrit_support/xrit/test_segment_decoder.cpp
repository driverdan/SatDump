#include "cli_test_helpers.h"
#include "xrit_support/xrit/fy4/segment_decoder.h"
#include "xrit_support/xrit/gk2a/segment_decoder.h"
#include "xrit_support/xrit/goes/segment_decoder.h"
#include "xrit_support/xrit/himawari/segment_decoder.h"
#include "xrit_support/xrit/msg/segment_decoder.h"
#include "xrit_support/xrit/segment_decoder.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <memory>
#include <tuple>

using namespace xrit_test;
namespace xr = satdump::xrit;

// The segmented decoders allocate the image their first segment's headers give. An image
// larger than SegmentedImageDecoder::MAX_IMAGE_PIXELS is not allocated: its segments are
// ignored, and the decoder logs it

namespace
{
    xr::XRITFile parsedFile(const Bytes &data)
    {
        xr::XRITFile f;
        f.lrit_data = data;
        f.parseHeaders();
        return f;
    }

    // Type 1, FY-4 specific: image structure and place of the segment in its image
    Bytes fy4ImageInformationRecord(uint8_t bits_per_pixel, uint16_t columns, uint16_t lines, uint8_t total_segments, uint8_t segment, uint16_t segment_line)
    {
        Bytes c(9 + 7, ' '); // Satellite and instrument names
        c.push_back(bits_per_pixel);
        put16(c, columns);
        put16(c, lines);
        c.push_back(0); // Compression
        c.push_back(1); // Channel
        c.push_back(total_segments);
        c.push_back(segment);
        c.push_back(segment);
        put16(c, segment_line);
        c.push_back(0); // Compression information
        return record(1, c);
    }

    // The Himawari decoder reads the segment number from the end of the name
    const std::string ABSURD = "ABSURD_SEGMENT_001";

    void checkIgnored(xr::SegmentedImageDecoder &decoder, xr::XRITFile &file, cli_test::LogCapture &log, const std::string &size)
    {
        INFO(log.dump());
        CHECK(decoder.image.size() == 0);
        CHECK(log.contains("File " + ABSURD + " gives an image of " + size + " pixels, more than the 1073741824 pixels a segmented image may have, its segments are ignored", slog::LOG_WARN));
        decoder.pushSegment(file);
        CHECK_FALSE(decoder.hasData());
        CHECK(decoder.image.size() == 0);
    }
} // namespace

TEST_CASE("SegmentedImageDecoder allows the images of every satellite", "[plugins][xrit_support][segment_decoder]")
{
    // Full disks at 0.5 km (Himawari AHI, GK-2A AMI, FY-4B AGRI, GOES-R ABI), MSG HRV (5568
    // columns, 24 segments of 464 lines), MSG and Elektro-L full disks, GOES-R HRIT full disk
    auto [width, height] = GENERATE(std::make_pair(22000, 22000), std::make_pair(21984, 21984), std::make_pair(21696, 21696), std::make_pair(5568, 24 * 464), std::make_pair(11136, 11136),
                                    std::make_pair(3712, 3712), std::make_pair(5424, 5424), std::make_pair(32768, 32768));
    CHECK(xr::SegmentedImageDecoder::imageSizeAllowed(width, height));
}

TEST_CASE("SegmentedImageDecoder does not allow an image larger than 32768 x 32768 pixels", "[plugins][xrit_support][segment_decoder]")
{
    CHECK_FALSE(xr::SegmentedImageDecoder::imageSizeAllowed(32768, 32769));
    CHECK_FALSE(xr::SegmentedImageDecoder::imageSizeAllowed(32769, 32768));
    CHECK_FALSE(xr::SegmentedImageDecoder::imageSizeAllowed(65535, 65535));
    CHECK_FALSE(xr::SegmentedImageDecoder::imageSizeAllowed(65535, (size_t)65535 * 65536));
    CHECK_FALSE(xr::SegmentedImageDecoder::imageSizeAllowed(SIZE_MAX, 2)); // Would overflow as a product
}

TEST_CASE("MSGSegmentedImageDecoder allocates a MSG HRV image", "[plugins][xrit_support][segment_decoder]")
{
    // 24 segments of 464 lines of 5568 pixels of 10 bits
    cli_test::LogCapture log;
    auto file = parsedFile(lritFile(0, {imageStructureRecord(10, 5568, 464, 0), annotationRecord("HRV.lrit"), msgSegmentIdentificationRecord(12, 3, 1, 24)}, Bytes(5568 * 464 * 2, 1)));
    xr::MSGSegmentedImageDecoder decoder(file);

    REQUIRE(decoder.image.size() == (size_t)5568 * 464 * 24);
    CHECK(decoder.image.width() == 5568);
    CHECK(decoder.image.height() == 464 * 24);
    CHECK(decoder.image.depth() == 16);
    CHECK_FALSE(log.contains("segments are ignored"));

    decoder.pushSegment(file);
    CHECK(decoder.hasData());
    CHECK(decoder.image.get(5568 * 464 * 2 - 1) == 0);
    CHECK(decoder.image.get(5568 * 464 * 2) == 0x0101);
}

TEST_CASE("MSGSegmentedImageDecoder ignores the segments of an image too large to allocate", "[plugins][xrit_support][segment_decoder]")
{
    // 65535 x 30000 pixels by 65536 segments, 250 TB: the allocation used to throw out of
    // the constructor
    cli_test::LogCapture log;
    auto file = parsedFile(lritFile(0, {imageStructureRecord(16, 65535, 30000, 0), annotationRecord(ABSURD), msgSegmentIdentificationRecord(1, 0, 0, 65535)}, Bytes(64, 1)));
    std::unique_ptr<xr::MSGSegmentedImageDecoder> decoder;
    REQUIRE_NOTHROW(decoder = std::make_unique<xr::MSGSegmentedImageDecoder>(file));
    checkIgnored(*decoder, file, log, "65535x1966080000");
}

TEST_CASE("FY4xSegmentedImageDecoder ignores the segments of an image too large to allocate", "[plugins][xrit_support][segment_decoder]")
{
    // 65535 x 65535 pixels by 255 segments, 2 TB
    cli_test::LogCapture log;
    auto file = parsedFile(lritFile(0, {fy4ImageInformationRecord(16, 65535, 65535, 255, 1, 0), annotationRecord(ABSURD)}, Bytes(64, 1)));
    std::unique_ptr<xr::FY4xSegmentedImageDecoder> decoder;
    REQUIRE_NOTHROW(decoder = std::make_unique<xr::FY4xSegmentedImageDecoder>(file));
    checkIgnored(*decoder, file, log, "65535x16711425");
}

// The three below would ask for 4 to 86 GB, which the allocator may give

TEST_CASE("HimawariSegmentedImageDecoder ignores the segments of an image too large to allocate", "[plugins][xrit_support][segment_decoder]")
{
    // 65535 x 65535 pixels by 10 segments
    cli_test::LogCapture log;
    auto file = parsedFile(lritFile(0, {imageStructureRecord(16, 65535, 65535, 0), annotationRecord(ABSURD)}, Bytes(64, 1)));
    xr::HimawariSegmentedImageDecoder decoder(file);
    checkIgnored(decoder, file, log, "65535x655350");
}

TEST_CASE("GK2ASegmentedImageDecoder ignores the segments of an image too large to allocate", "[plugins][xrit_support][segment_decoder]")
{
    // The GK-2A image is columns x columns
    cli_test::LogCapture log;
    auto file = parsedFile(lritFile(0, {imageStructureRecord(16, 65535, 1000, 0), annotationRecord(ABSURD)}, Bytes(64, 1)));
    xr::GK2ASegmentedImageDecoder decoder(file);
    checkIgnored(decoder, file, log, "65535x65535");
}

TEST_CASE("GOESSegmentedImageDecoder ignores the segments of an image too large to allocate", "[plugins][xrit_support][segment_decoder]")
{
    // The image is max_column x max_row, or max_column x max_segment * lines without a max_row
    cli_test::LogCapture log;
    auto [max_row, lines, size] = GENERATE(std::make_tuple(65535, 4, std::string("65535x65535")), std::make_tuple(0, 65535, std::string("65535x4294836225")));
    auto file = parsedFile(
        lritFile(0, {imageStructureRecord(8, 16, lines, 0), annotationRecord(ABSURD), noaaRecord(16, 13, 0, 0), segmentIdentificationRecord(1, 0, 0, 0, 65535, 65535, max_row)}, Bytes(64, 1)));
    xr::GOESSegmentedImageDecoder decoder(file);
    checkIgnored(decoder, file, log, size);
}
