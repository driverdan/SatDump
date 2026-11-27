#include "image/image_test_helpers.h"
#include "image/jpeg12_utils.h"
#include "image/jpeg_test_helpers.h"
#include "image/jpeg_utils.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using jpeg_test::encodeJpeg;
using jpeg_test::makeSamples;
using satdump::image::Image;

// The round-trip with save_jpeg_mem is tested in io/test_jpegio.cpp

TEST_CASE("image::decompress_jpeg decodes lossless 8-bit data exactly", "[image][jpeg_utils]")
{
    const int w = 21, h = 13;
    auto samples = makeSamples(w, h, 8);
    auto data = encodeJpeg(samples, w, h, 8, true);
    REQUIRE(!data.empty());

    Image out = satdump::image::decompress_jpeg(data.data(), data.size());
    REQUIRE(out.depth() == 8);
    REQUIRE(out.channels() == 1);
    REQUIRE(out.width() == size_t(w));
    REQUIRE(out.height() == size_t(h));
    for (int i = 0; i < w * h; i++)
        REQUIRE(out.get(i) == samples[i]);
}

TEST_CASE("image::decompress_jpeg scales data of more than 8 bits down to 8", "[image][jpeg_utils]")
{
    auto precision = GENERATE(10, 12, 16);
    INFO("precision " << precision);
    const int w = 21, h = 13;
    auto samples = makeSamples(w, h, precision);
    auto data = encodeJpeg(samples, w, h, precision, true);
    REQUIRE(!data.empty());

    Image out = satdump::image::decompress_jpeg(data.data(), data.size());
    REQUIRE(out.depth() == 8);
    REQUIRE(out.width() == size_t(w));
    REQUIRE(out.height() == size_t(h));
    for (int i = 0; i < w * h; i++)
        REQUIRE(out.get(i) == samples[i] >> (precision - 8));
}

TEST_CASE("image::decompress_jpeg converts colour JPEGs to a single channel", "[image][jpeg_utils]")
{
    // All components used to be written into the single-channel buffer, overrunning it
    const int w = 40, h = 24;
    std::vector<int> samples(w * h * 3);
    for (int i = 0; i < w * h; i++)
    {
        samples[i * 3 + 0] = 200;
        samples[i * 3 + 1] = 100;
        samples[i * 3 + 2] = 50;
    }
    auto data = encodeJpeg(samples, w, h, 8, false, 3);
    REQUIRE(!data.empty());

    Image out = satdump::image::decompress_jpeg(data.data(), data.size());
    REQUIRE(out.channels() == 1);
    REQUIRE(out.width() == size_t(w));
    REQUIRE(out.height() == size_t(h));
    // Luma of (200, 100, 50)
    for (int i = 0; i < w * h; i++)
        REQUIRE(std::abs(out.get(i) - 124) <= 3);
}

TEST_CASE("image::decompress_jpeg returns an empty image for lossless colour JPEGs", "[image][jpeg_utils]")
{
    // libjpeg does not convert the colours of lossless images to grayscale
    const int w = 20, h = 12;
    std::vector<int> samples(w * h * 3, 100);
    auto data = encodeJpeg(samples, w, h, 8, true, 3);
    REQUIRE(!data.empty());

    Image out = satdump::image::decompress_jpeg(data.data(), data.size());
    CHECK(out.size() == 0);
    Image out12 = satdump::image::decompress_jpeg12(data.data(), data.size());
    CHECK(out12.size() == 0);
}

TEST_CASE("image::decompress_jpeg decodes a truncated JPEG", "[image][jpeg_utils]")
{
    // libjpeg pads the missing data with a warning, as received files can be cut short
    auto ignore_errors = GENERATE(false, true);
    const int w = 64, h = 64;
    auto samples = makeSamples(w, h, 8);
    auto data = encodeJpeg(samples, w, h, 8, false);
    REQUIRE(!data.empty());
    data.resize(data.size() / 2);

    Image out = satdump::image::decompress_jpeg(data.data(), data.size(), ignore_errors);
    REQUIRE(out.width() == size_t(w));
    REQUIRE(out.height() == size_t(h));
    // The first rows are intact
    for (int x = 0; x < w; x++)
        REQUIRE(std::abs(out.get(x) - samples[x]) <= 8);
}

TEST_CASE("image::decompress_jpeg_samples keeps the samples and reports the precision", "[image][jpeg_utils]")
{
    auto precision = GENERATE(4, 8, 12, 16);
    INFO("precision " << precision);
    const int w = 17, h = 9;
    auto samples = makeSamples(w, h, precision);
    auto data = encodeJpeg(samples, w, h, precision, true);
    REQUIRE(!data.empty());

    int got_precision = -1;
    Image out = satdump::image::decompress_jpeg_samples(data.data(), data.size(), false, got_precision);
    CHECK(got_precision == precision);
    REQUIRE(out.depth() == 16);
    REQUIRE(out.width() == size_t(w));
    REQUIRE(out.height() == size_t(h));
    for (int i = 0; i < w * h; i++)
        REQUIRE(out.get(i) == samples[i]);

    std::vector<uint8_t> junk = {0x10, 0x20, 0x30, 0x40};
    Image bad = satdump::image::decompress_jpeg_samples(junk.data(), junk.size(), false, got_precision);
    CHECK(got_precision == 0);
    CHECK(bad.size() == 0);
}
