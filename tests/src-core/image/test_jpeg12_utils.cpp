#include "image/image_test_helpers.h"
#include "image/jpeg12_utils.h"
#include "image/jpeg_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using jpeg_test::encodeJpeg;
using jpeg_test::makeSamples;
using satdump::image::Image;

TEST_CASE("image::decompress_jpeg12 decodes lossless 12-bit data scaled to 16 bits", "[image][jpeg12_utils]")
{
    const int w = 24, h = 16;
    auto samples = makeSamples(w, h, 12);
    auto data = encodeJpeg(samples, w, h, 12, true);
    REQUIRE(!data.empty());

    Image out = satdump::image::decompress_jpeg12(data.data(), data.size());
    REQUIRE(out.depth() == 16);
    REQUIRE(out.channels() == 1);
    REQUIRE(out.width() == size_t(w));
    REQUIRE(out.height() == size_t(h));
    for (int i = 0; i < w * h; i++)
        REQUIRE(out.get(i) == samples[i] << 4);
}

TEST_CASE("image::decompress_jpeg12 decodes lossy 12-bit data within tolerance", "[image][jpeg12_utils]")
{
    const int w = 32, h = 16;
    std::vector<int> samples(w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            samples[y * w + x] = x * 100 + y * 20; // smooth ramp
    auto data = encodeJpeg(samples, w, h, 12, false);
    REQUIRE(!data.empty());

    Image out = satdump::image::decompress_jpeg12(data.data(), data.size());
    REQUIRE(out.width() == size_t(w));
    REQUIRE(out.height() == size_t(h));
    for (int i = 0; i < w * h; i++)
        REQUIRE(std::abs((out.get(i) >> 4) - samples[i]) <= 40);
}

TEST_CASE("image::decompress_jpeg12 scales lossless data of up to 12 bits like 12-bit data", "[image][jpeg12_utils]")
{
    // libjpeg-turbo decodes these with its 8-bit or its 12-bit API. GK-2A LRIT
    // sends 10-bit images, which its decoder scales further itself
    auto precision = GENERATE(2, 7, 8, 9, 10, 11);
    INFO("precision " << precision);
    const int w = 19, h = 11;
    auto samples = makeSamples(w, h, precision);
    auto data = encodeJpeg(samples, w, h, precision, true);
    REQUIRE(!data.empty());

    Image out = satdump::image::decompress_jpeg12(data.data(), data.size());
    REQUIRE(out.depth() == 16);
    REQUIRE(out.width() == size_t(w));
    REQUIRE(out.height() == size_t(h));
    for (int i = 0; i < w * h; i++)
        REQUIRE(out.get(i) == samples[i] << 4);
}

TEST_CASE("image::decompress_jpeg12 scales lossless data of more than 12 bits down to 12", "[image][jpeg12_utils]")
{
    auto precision = GENERATE(13, 16);
    INFO("precision " << precision);
    const int w = 19, h = 11;
    auto samples = makeSamples(w, h, precision);
    auto data = encodeJpeg(samples, w, h, precision, true);
    REQUIRE(!data.empty());

    Image out = satdump::image::decompress_jpeg12(data.data(), data.size());
    REQUIRE(out.width() == size_t(w));
    REQUIRE(out.height() == size_t(h));
    for (int i = 0; i < w * h; i++)
        REQUIRE(out.get(i) == (samples[i] >> (precision - 12)) << 4);
}

TEST_CASE("image::decompress_jpeg12 returns an empty image on invalid data", "[image][jpeg12_utils]")
{
    std::vector<uint8_t> junk = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    Image out = satdump::image::decompress_jpeg12(junk.data(), junk.size());
    CHECK(out.width() == 0);
    CHECK(out.size() == 0);
}
