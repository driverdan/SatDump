#include "elektro_arktika_support/elektro_arktika/instruments/msugs/msu_vis_reader.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
    // pushFrame() reads the pixels up to byte 15244 (190 + 3011 * 5 - 1), so the frame is
    // made long enough for that
    const size_t FRAME_SIZE = 15245;
} // namespace

// Bytes 10 to 14 of a VIS frame are a 40-bit big-endian time, in 1/256 s
TEST_CASE("MSUVISReader reads the 40-bit line time", "[plugins][elektro_arktika_support][msugs]")
{
    elektro_arktika::msugs::MSUVISReader reader;
    std::vector<uint8_t> frame(FRAME_SIZE, 0);
    const int counter = 5;
    frame[8] = counter >> 8;
    frame[9] = counter & 0xFF;

    // The top byte must land above bit 32, and byte 11 is >= 0x80, so it must not be sign extended
    frame[10] = 0x12;
    frame[11] = 0x9A;
    frame[12] = 0xBC;
    frame[13] = 0xDE;
    frame[14] = 0xF0;

    reader.pushFrame(frame.data(), false);

    // 0x129ABCDEF0 = 79905480432, / 256 = 312130782.9375, then the reader's fixed offset
    double expected = 312130782.9375 + 1735204808.2837029 - 1800 - 88;
    REQUIRE(reader.frames == 1);
    CHECK_THAT(reader.timestamps[counter], WithinAbs(expected, 1e-6));
}
