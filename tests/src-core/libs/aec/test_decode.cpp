#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "helpers/compare.h"
#include "helpers/random.h"

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

    // What aec_decode_init returns for a 8 bit stream. The decoder is freed again if it was made
    int decodeInit(unsigned int block_size, unsigned int rsi, unsigned int flags)
    {
        aec_stream strm = {};
        strm.bits_per_sample = 8;
        strm.block_size = block_size;
        strm.rsi = rsi;
        strm.flags = flags;
        int status = aec_decode_init(&strm);
        if (status == AEC_OK)
            aec_decode_end(&strm);
        return status;
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
    CHECK(test_helpers::bytesDiff(out, line) == "");
}

TEST_CASE("SZ_BufftoBuffDecompress rejects the block sizes libaec does not support", "[libs][aec]")
{
    // 1 and odd sizes wrote past the output and RSI buffers (#36)
    int pixels_per_block = GENERATE(1, 2, 3, 4, 5, 7, 9, 12, 24, 33, 100, 128, 255);
    SZ_com_t p = lineSettings(pixels_per_block, 16);

    int rejected = 0;
    for (uint32_t seed = 1; seed <= 50; seed++)
    {
        std::vector<uint8_t> in = test_helpers::randomBytes(64, seed); // Not from the Rice encoder
        std::vector<uint8_t> out(16);
        size_t out_size = out.size();
        if (SZ_BufftoBuffDecompress(out.data(), &out_size, in.data(), in.size(), &p) == SZ_PARAM_ERROR)
            rejected++;
    }
    CHECK(rejected == 50);
}

TEST_CASE("SZ_BufftoBuffDecompress rejects a scanline of 0 pixels", "[libs][aec]")
{
    // It gives the decoder a RSI of 0 blocks, and so no RSI buffer to decode to
    SZ_com_t p = lineSettings(16, 0);
    std::vector<uint8_t> in = test_helpers::randomBytes(64, 1);
    std::vector<uint8_t> out(1);
    size_t out_size = 0; // As the GOES HRIT decoder does for an image of 0 columns
    CHECK(SZ_BufftoBuffDecompress(out.data(), &out_size, in.data(), in.size(), &p) == SZ_PARAM_ERROR);
}

TEST_CASE("aec_decode_init rejects a RSI of 0 blocks", "[libs][aec]")
{
    CHECK(decodeInit(16, 0, 0) == AEC_CONF_ERROR);
    CHECK(decodeInit(16, 0, AEC_NOT_ENFORCE) == AEC_CONF_ERROR);
    CHECK(decodeInit(16, 1, 0) == AEC_OK);
}

TEST_CASE("aec_decode_init allows the even block sizes up to 256 with AEC_NOT_ENFORCE", "[libs][aec]")
{
    CHECK(decodeInit(0, 1, AEC_NOT_ENFORCE) == AEC_CONF_ERROR);
    CHECK(decodeInit(3, 1, AEC_NOT_ENFORCE) == AEC_CONF_ERROR);
    CHECK(decodeInit(258, 1, AEC_NOT_ENFORCE) == AEC_CONF_ERROR);

    CHECK(decodeInit(2, 1, AEC_NOT_ENFORCE) == AEC_OK);
    CHECK(decodeInit(12, 1, AEC_NOT_ENFORCE) == AEC_OK);
    CHECK(decodeInit(256, 1, AEC_NOT_ENFORCE) == AEC_OK);

    // Without the flag, only the sizes of the standard
    CHECK(decodeInit(12, 1, 0) == AEC_CONF_ERROR);
    CHECK(decodeInit(0, 1, 0) == AEC_CONF_ERROR);
}
