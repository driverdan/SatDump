#include "cli_test_helpers.h"
#include "helpers/compare.h"
#include "xrit_support/xrit/msg/decomp.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>

using namespace xrit_test;
using test_helpers::bytesDiff;
namespace xr = satdump::xrit;

TEST_CASE("decompressMsgHritFileIfRequired discards a wavelet compressed image of 0 bits per pixel", "[plugins][xrit_support][msg_decomp]")
{
    // The decompressed size is divided by the bits per pixel
    cli_test::LogCapture log;
    Bytes headers = lritHeaders(0, {imageStructureRecord(0, 16, 4, 1), annotationRecord("MSG_SEGMENT.lrit")}, 64);
    xr::XRITFile file;
    file.lrit_data = headers;
    file.lrit_data.resize(headers.size() + 64, 0x55);
    file.parseHeaders();

    xr::msg::decompressMsgHritFileIfRequired(file);

    // The headers are kept, and the image is black, as when the decompression fails
    Bytes expected = headers;
    expected.resize(headers.size() + 16 * 4, 0);
    CHECK(bytesDiff(file.lrit_data, expected) == "");
    CHECK(log.contains("File MSG_SEGMENT.lrit has 0 bits per pixel in its image structure record, it cannot be decompressed", slog::LOG_WARN));
}
