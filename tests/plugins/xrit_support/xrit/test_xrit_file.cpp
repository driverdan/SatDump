#include "xrit_support/xrit/goes/goes_headers.h"
#include "xrit_support/xrit/msg/msg_headers.h"
#include "xrit_support/xrit/xrit_file.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace xrit_test;
namespace xr = satdump::xrit;

namespace
{
    xr::XRITFile fileOf(const Bytes &data)
    {
        xr::XRITFile f;
        f.lrit_data = data;
        return f;
    }

    // The file is cut after `size` bytes. The vector keeps its memory past the end, filled with
    // 0xFF, so that reading past the data gives wrong values rather than a crash
    xr::XRITFile cutFileOf(const Bytes &data, size_t size)
    {
        xr::XRITFile f;
        f.lrit_data = data;
        f.lrit_data.resize(size);
        f.lrit_data.resize(data.size() + 64, 0xFF);
        f.lrit_data.resize(size);
        return f;
    }
} // namespace

TEST_CASE("XRITFile::parseHeaders finds every header record", "[plugins][xrit_support][xrit_file]")
{
    Bytes data = {1, 2, 3, 4, 5};
    auto f = fileOf(lritFile(0, {imageStructureRecord(8, 64, 32, 1), annotationRecord("IMAGE.lrit"), timeStampRecord(24000, 43200000)}, data));
    f.parseHeaders();

    // Offsets of the records in the file
    CHECK(f.all_headers == std::map<int, int>{{0, 0}, {1, 16}, {4, 25}, {5, 38}});
    CHECK(f.total_header_length == 16 + 9 + 13 + 10);
    CHECK(f.filename == "IMAGE.lrit");

    CHECK(f.hasHeader<xr::PrimaryHeader>());
    CHECK(f.hasHeader<xr::ImageStructureRecord>());
    CHECK(f.hasHeader<xr::AnnotationRecord>());
    CHECK(f.hasHeader<xr::TimeStampRecord>());
    CHECK_FALSE(f.hasHeader<xr::ImageNavigationRecord>());
    CHECK_FALSE(f.hasHeader<xr::ImageDataFunctionRecord>());

    auto primary = f.getHeader<xr::PrimaryHeader>();
    CHECK(primary.type == 0);
    CHECK(primary.record_length == 16);
    CHECK(primary.file_type_code == 0);
    CHECK(primary.total_header_length == 48);
    CHECK(primary.data_field_length == data.size() * 8);

    auto image = f.getHeader<xr::ImageStructureRecord>();
    CHECK(image.record_length == 9);
    CHECK(image.bit_per_pixel == 8);
    CHECK(image.columns_count == 64);
    CHECK(image.lines_count == 32);
    CHECK(image.compression_flag == 1);

    CHECK(f.getHeader<xr::AnnotationRecord>().annotation_text == "IMAGE.lrit");
}

TEST_CASE("XRITFile headers carry the fields of the LRIT standard", "[plugins][xrit_support][xrit_file]")
{
    auto f = fileOf(lritFile(0, {imageNavigationRecord("GEOS(-75.2)", 0x00120034, 0x00560078, -1000, 2000), textRecord(3, "_NAME:=x\r\n")}, {}));
    f.parseHeaders();

    auto nav = f.getHeader<xr::ImageNavigationRecord>();
    CHECK(nav.record_length == 51);
    CHECK(nav.projection_name.substr(0, 11) == "GEOS(-75.2)");
    CHECK(nav.projection_name.size() == 32);
    CHECK(nav.column_scaling_factor == 0x00120034);
    CHECK(nav.line_scaling_factor == 0x00560078);
    CHECK(nav.column_offset == -1000.0);
    CHECK(nav.line_offset == 2000.0);
    CHECK(nav.column_scalar == 0.0); // Only set later, by the satellite specific code

    CHECK(f.getHeader<xr::ImageDataFunctionRecord>().datas == "_NAME:=x\r\n");
    CHECK(f.filename == ""); // No annotation
}

TEST_CASE("XRITFile::parseHeaders makes a safe file name of the annotation", "[plugins][xrit_support][xrit_file]")
{
    auto f = fileOf(lritFile(2, {annotationRecord("../a\\b c\td.lrit")}, {}));
    f.parseHeaders();
    CHECK(f.filename == ".._a_b_c_d.lrit");
}

TEST_CASE("XRITFile::parseHeaders keeps the first record of each type", "[plugins][xrit_support][xrit_file]")
{
    auto f = fileOf(lritFile(2, {annotationRecord("first.lrit"), annotationRecord("second.lrit")}, {}));
    f.parseHeaders();
    CHECK(f.all_headers[4] == 16);
    CHECK(f.filename == "first.lrit");
}

TEST_CASE("XRITFile::parseHeaders stops at a record without a length", "[plugins][xrit_support][xrit_file]")
{
    auto data = lritFile(2, {annotationRecord("a.lrit"), annotationRecord("never_reached")}, {});
    data[16 + 9 + 1] = 0, data[16 + 9 + 2] = 0; // Length of the second annotation
    auto f = fileOf(data);

    f.parseHeaders();
    CHECK(f.all_headers.size() == 2);
    CHECK(f.filename == "a.lrit");
}

TEST_CASE("XRITFile::parseHeaders in safe mode ignores records that end past the file", "[plugins][xrit_support][xrit_file]")
{
    auto data = lritFile(2, {annotationRecord("a.lrit"), ancillaryTextRecord("k = v;")}, {});
    data.resize(data.size() - 4); // The ancillary record is cut
    auto f = fileOf(data);

    f.parseHeaders(true);
    CHECK(f.hasHeader<xr::AnnotationRecord>());
    CHECK(f.all_headers.count(6) == 0);
    CHECK(f.filename == "a.lrit");
}

TEST_CASE("XRITFile::parseHeaders forgets the headers of the previous file", "[plugins][xrit_support][xrit_file]")
{
    auto f = fileOf(lritFile(0, {imageStructureRecord(8, 4, 4, 0), annotationRecord("image.lrit")}, {}));
    f.parseHeaders();
    REQUIRE(f.hasHeader<xr::ImageStructureRecord>());

    f.lrit_data = lritFile(2, {annotationRecord("text.lrit")}, {});
    f.parseHeaders();
    CHECK_FALSE(f.hasHeader<xr::ImageStructureRecord>());
    CHECK(f.filename == "text.lrit");
    CHECK(f.total_header_length == 16 + 12);
}

TEST_CASE("XRITFile::getHeader reads what a header-only file lacks of a record as zeros", "[plugins][xrit_support][xrit_file]")
{
    SECTION("Record cut by the end of the file")
    {
        auto data = lritFile(2, {imageNavigationRecord("GEOS(-75.2)", 0x00120034, 0x00560078, -1000, 2000)}, {});
        auto f = cutFileOf(data, 16 + 20);
        f.parseHeaders();
        REQUIRE(f.hasHeader<xr::ImageNavigationRecord>());

        auto nav = f.getHeader<xr::ImageNavigationRecord>();
        CHECK(nav.record_length == 51);
        CHECK(nav.projection_name == std::string("GEOS(-75.2)      ") + std::string(15, '\0'));
        CHECK(nav.column_scaling_factor == 0);
        CHECK(nav.line_scaling_factor == 0);
        CHECK(nav.column_offset == 0.0);
        CHECK(nav.line_offset == 0.0);
    }

    SECTION("Last record shorter than its type")
    {
        auto data = lritFile(2, {record(2, Bytes(7, 'x')), record(129, {'N', 'O', 'A', 'A', 0, 7})}, {});
        auto f = cutFileOf(data, data.size());
        f.parseHeaders();
        REQUIRE(f.hasHeader<xr::goes::NOAALRITHeader>());

        auto noaa = f.getHeader<xr::goes::NOAALRITHeader>();
        CHECK(noaa.record_length == 9);
        CHECK(std::string(noaa.agency_signature, 4) == "NOAA");
        CHECK(noaa.product_id == 7);
        CHECK(noaa.product_subid == 0);
        CHECK(noaa.parameter == 0);
        CHECK(noaa.noaa_specific_compression == 0);
    }
}

TEST_CASE("XRITFile::parseHeaders stops where the headers go past the data", "[plugins][xrit_support][xrit_file]")
{
    auto data = lritFile(2, {annotationRecord("a.lrit"), annotationRecord("bbbb")}, {});
    auto f = cutFileOf(data, 16 + 9); // Cut right after the first annotation
    bool safe = GENERATE(false, true);

    f.parseHeaders(safe);
    CHECK(f.all_headers == std::map<int, int>{{0, 0}, {4, 16}});
    CHECK(f.filename == "a.lrit");
}

TEST_CASE("XRITFile::parseHeaders reads a file shorter than a primary header as empty", "[plugins][xrit_support][xrit_file]")
{
    size_t size = GENERATE(0, 3);
    xr::XRITFile f;
    f.lrit_data = lritFile(2, {}, {}); // A file with no record...
    f.lrit_data.resize(size);          // ...is cut, and the rest of its primary header stays in memory past the data

    f.parseHeaders();
    CHECK(f.all_headers.empty());
    CHECK(f.total_header_length == 0);
    CHECK(f.getHeader<xr::PrimaryHeader>().record_length == (size >= 3 ? 16 : 0));
}

TEST_CASE("XRITFile::getHeader reads a record from a file shorter than 3 bytes as zeros", "[plugins][xrit_support][xrit_file]")
{
    size_t size = GENERATE(1, 2);
    auto f = cutFileOf({4, 0x01, 0x02}, size); // Record length 0x0100 when the third byte is missing

    f.parseHeaders();
    REQUIRE(f.all_headers.empty());

    // The primary header is always read at offset 0
    auto primary = f.getHeader<xr::PrimaryHeader>();
    CHECK(primary.type == 4);
    CHECK(primary.record_length == (size == 2 ? 0x0100 : 0));
    CHECK(primary.total_header_length == 0);
    CHECK(primary.data_field_length == 0);

    // The file has no annotation
    auto annotation = f.getHeader<xr::AnnotationRecord>();
    CHECK(annotation.record_length == 0);
    CHECK(annotation.annotation_text == "");
}

TEST_CASE("XRITFile::parseHeaders stops at a record too short to hold its own length", "[plugins][xrit_support][xrit_file]")
{
    uint8_t length = GENERATE(1, 2);
    auto f = fileOf(lritFile(2, {annotationRecord("a.lrit"), Bytes{3, 0, length}, annotationRecord("b.lrit")}, {}));

    f.parseHeaders();
    CHECK(f.all_headers == std::map<int, int>{{0, 0}, {4, 16}});
    CHECK(f.filename == "a.lrit");
}

TEST_CASE("XRITFile text records with a length under 3 are empty", "[plugins][xrit_support][xrit_file]")
{
    Bytes data = {0, 0, 2, 'a', 'b', 'c'};
    CHECK(xr::ImageDataFunctionRecord(data.data()).datas == "");
    CHECK(xr::AnnotationRecord(data.data()).annotation_text == "");
    CHECK(xr::goes::AnnotationRecord(data.data()).annotation_text == "");
    CHECK(xr::goes::AncillaryTextRecord(data.data()).ancillary_text == "");
}

TEST_CASE("XRITFile::parseHeaders in safe mode keeps the last record of a header-only file", "[plugins][xrit_support][xrit_file]")
{
    auto f = fileOf(lritFile(2, {annotationRecord("a.lrit")}, {}));
    f.parseHeaders(true);
    CHECK(f.all_headers == std::map<int, int>{{0, 0}, {4, 16}});
    CHECK(f.filename == "a.lrit");
}

TEST_CASE("XRITFile::getHeader reads a record the file does not have as zeros, and leaves it absent", "[plugins][xrit_support][xrit_file]")
{
    // Read from the primary header instead, the segment number and the product ID would hold
    // bytes of its total header length
    auto f = fileOf(lritFile(0, {imageStructureRecord(8, 64, 32, 0), annotationRecord("IMAGE.lrit")}, Bytes(64 * 32, 1)));
    f.parseHeaders();
    REQUIRE(f.all_headers == std::map<int, int>{{0, 0}, {1, 16}, {4, 25}});

    auto segment = f.getHeader<xr::msg::SegmentIdentificationHeader>();
    CHECK(segment.type == 0);
    CHECK(segment.record_length == 0);
    CHECK(segment.channel_id == 0);
    CHECK(segment.segment_sequence_number == 0);
    CHECK(segment.planned_start_segment == 0);
    CHECK(segment.planned_end_segment == 0);

    auto noaa = f.getHeader<xr::goes::NOAALRITHeader>();
    CHECK(noaa.record_length == 0);
    CHECK(noaa.product_id == 0);
    CHECK(noaa.product_subid == 0);
    CHECK(noaa.noaa_specific_compression == 0);

    auto ancillary = f.getHeader<xr::goes::AncillaryTextRecord>();
    CHECK(ancillary.ancillary_text == "");
    CHECK(ancillary.meta.empty());

    CHECK_FALSE(f.hasHeader<xr::msg::SegmentIdentificationHeader>());
    CHECK_FALSE(f.hasHeader<xr::goes::NOAALRITHeader>());
    CHECK_FALSE(f.hasHeader<xr::goes::AncillaryTextRecord>());
    CHECK(f.all_headers == std::map<int, int>{{0, 0}, {1, 16}, {4, 25}});

    // The records the file has are still read from their own bytes
    CHECK(f.getHeader<xr::ImageStructureRecord>().columns_count == 64);
    CHECK(f.getHeader<xr::AnnotationRecord>().annotation_text == "IMAGE.lrit");
}
