#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <vector>

extern "C"
{
#include "libs/aec/libaec.h"
#include "libs/aec/szlib.h"
}

namespace
{
    // The settings the GOES HRIT decoder uses for one 8 bit image line
    SZ_com_t lineSettings(int pixels_per_block, int columns)
    {
        SZ_com_t p;
        p.options_mask = SZ_ALLOW_K13_OPTION_MASK | SZ_MSB_OPTION_MASK | SZ_NN_OPTION_MASK | SZ_RAW_OPTION_MASK;
        p.bits_per_pixel = 8;
        p.pixels_per_block = pixels_per_block;
        p.pixels_per_scanline = columns;
        return p;
    }

    // Bytes that do not come from the Rice encoder
    std::vector<uint8_t> noise(size_t size, uint32_t seed)
    {
        std::vector<uint8_t> out(size);
        for (auto &b : out)
        {
            seed = seed * 1103515245 + 12345;
            b = seed >> 16;
        }
        return out;
    }
} // namespace

TEST_CASE("SZ_BufftoBuffDecompress decodes the block sizes libaec supports", "[libs][aec]")
{
    int pixels_per_block = GENERATE(8, 16, 32, 64);
    std::vector<uint8_t> line(100);
    for (size_t i = 0; i < line.size(); i++)
        line[i] = i * 7;

    SZ_com_t p = lineSettings(pixels_per_block, line.size());
    std::vector<uint8_t> compressed(line.size() * 2 + 64);
    size_t compressed_size = compressed.size();
    REQUIRE(SZ_BufftoBuffCompress(compressed.data(), &compressed_size, line.data(), line.size(), &p) == SZ_OK);

    std::vector<uint8_t> out(line.size());
    size_t out_size = out.size();
    REQUIRE(SZ_BufftoBuffDecompress(out.data(), &out_size, compressed.data(), compressed_size, &p) == SZ_OK);
    CHECK(out_size == line.size());
    CHECK(out == line);
}

TEST_CASE("SZ_BufftoBuffDecompress rejects the block sizes libaec does not support", "[libs][aec]")
{
    // 1 and odd sizes wrote past the output and RSI buffers (#36)
    int pixels_per_block = GENERATE(1, 2, 3, 4, 5, 7, 9, 12, 24, 33, 100, 128, 255);
    SZ_com_t p = lineSettings(pixels_per_block, 16);

    for (uint32_t seed = 1; seed <= 50; seed++)
    {
        std::vector<uint8_t> in = noise(64, seed);
        std::vector<uint8_t> out(16);
        size_t out_size = out.size();
        CHECK(SZ_BufftoBuffDecompress(out.data(), &out_size, in.data(), in.size(), &p) == SZ_PARAM_ERROR);
    }
}

TEST_CASE("aec_decode_init allows any even block size with AEC_NOT_ENFORCE", "[libs][aec]")
{
    aec_stream strm = {};
    strm.bits_per_sample = 8;
    strm.rsi = 1;
    strm.flags = AEC_NOT_ENFORCE;

    strm.block_size = 3;
    CHECK(aec_decode_init(&strm) == AEC_CONF_ERROR);

    strm.block_size = 12;
    REQUIRE(aec_decode_init(&strm) == AEC_OK);
    aec_decode_end(&strm);
}
