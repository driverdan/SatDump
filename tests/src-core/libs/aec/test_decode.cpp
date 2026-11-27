#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "helpers/compare.h"
#include "helpers/random.h"

#include <algorithm>
#include <cstdint>
#include <tuple>
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

namespace
{
    struct AecConfig
    {
        unsigned int bits_per_sample;
        unsigned int block_size;
        unsigned int rsi;
        unsigned int flags;
    };

    aec_stream makeStream(const AecConfig &c)
    {
        aec_stream strm = {};
        strm.bits_per_sample = c.bits_per_sample;
        strm.block_size = c.block_size;
        strm.rsi = c.rsi;
        strm.flags = c.flags;
        return strm;
    }

    size_t bytesPerSample(const AecConfig &c)
    {
        if (c.bits_per_sample > 16)
            return (c.bits_per_sample <= 24 && (c.flags & AEC_DATA_3BYTE)) ? 3 : 4;
        return c.bits_per_sample > 8 ? 2 : 1;
    }

    // Unsigned samples of one kind, so that the encoder uses each of its block options
    std::vector<uint8_t> makeSamples(const AecConfig &c, size_t count, int kind, test_helpers::Lcg &rng)
    {
        size_t bytes = bytesPerSample(c);
        uint32_t mask = c.bits_per_sample == 32 ? UINT32_MAX : (UINT32_C(1) << c.bits_per_sample) - 1;
        uint32_t v = rng.next();
        std::vector<uint8_t> out(count * bytes);
        for (size_t i = 0; i < count; i++)
        {
            uint32_t r = rng.next();
            uint32_t x;
            if (kind == 0) // Noise: uncompressed blocks
                x = r;
            else if (kind == 1) // Smooth: split blocks
                x = v += (r >> 8) % 9 - 4;
            else if (kind == 2) // Sparse: second extension and zero blocks
                x = (r >> 8) % 32 == 0 ? (r >> 16) % 3 : 0;
            else // Long runs of zeros: zero blocks up to the end of a segment
                x = (i / 300) % 2 ? r : 0;
            x &= mask;
            for (size_t b = 0; b < bytes; b++)
                out[i * bytes + b] = x >> ((c.flags & AEC_DATA_MSB) ? 8 * (bytes - 1 - b) : 8 * b);
        }
        return out;
    }

    // The samples as the decoder writes signed samples with preprocessing: sign extended to the
    // size they are stored in. The encoder takes them as bits_per_sample bit two's complement.
    std::vector<uint8_t> signExtended(const AecConfig &c, std::vector<uint8_t> samples)
    {
        size_t bytes = bytesPerSample(c);
        bool msb = c.flags & AEC_DATA_MSB;
        for (size_t i = 0; i < samples.size(); i += bytes)
        {
            uint32_t x = 0;
            for (size_t b = 0; b < bytes; b++)
                x |= (uint32_t)samples[i + b] << (msb ? 8 * (bytes - 1 - b) : 8 * b);
            if (c.bits_per_sample < 32 && (x >> (c.bits_per_sample - 1)) & 1)
                x |= ~((UINT32_C(1) << c.bits_per_sample) - 1);
            for (size_t b = 0; b < bytes; b++)
                samples[i + b] = x >> (msb ? 8 * (bytes - 1 - b) : 8 * b);
        }
        return samples;
    }

    std::vector<uint8_t> encode(const AecConfig &c, const std::vector<uint8_t> &samples)
    {
        std::vector<uint8_t> out(samples.size() * 2 + 1024);
        aec_stream strm = makeStream(c);
        strm.next_in = samples.data();
        strm.avail_in = samples.size();
        strm.next_out = out.data();
        strm.avail_out = out.size();
        REQUIRE(aec_buffer_encode(&strm) == AEC_OK);
        out.resize(strm.total_out);
        return out;
    }

    // Decodes in one call, with the input in a buffer of its exact size
    bool decodesTo(const AecConfig &c, const std::vector<uint8_t> &encoded, const std::vector<uint8_t> &expected)
    {
        std::vector<uint8_t> out(expected.size());
        aec_stream strm = makeStream(c);
        strm.next_in = encoded.data();
        strm.avail_in = encoded.size();
        strm.next_out = out.data();
        strm.avail_out = out.size();
        return aec_buffer_decode(&strm) == AEC_OK && strm.total_out == out.size() && out == expected;
    }

    // Decodes through calls given a few bytes of input and room for a few samples, as a stream would
    bool decodesInPiecesTo(const AecConfig &c, const std::vector<uint8_t> &encoded, const std::vector<uint8_t> &expected, test_helpers::Lcg &rng)
    {
        std::vector<uint8_t> out(expected.size());
        aec_stream strm = makeStream(c);
        if (aec_decode_init(&strm) != AEC_OK)
            return false;
        size_t in_pos = 0, out_pos = 0;
        bool ok = true;
        while (ok && out_pos < out.size())
        {
            size_t in_size = std::min<size_t>(1 + rng.below(40), encoded.size() - in_pos);
            size_t out_size = std::min<size_t>((1 + rng.below(40)) * bytesPerSample(c), out.size() - out_pos);
            strm.next_in = encoded.data() + in_pos;
            strm.avail_in = in_size;
            strm.next_out = out.data() + out_pos;
            strm.avail_out = out_size;
            ok = aec_decode(&strm, AEC_NO_FLUSH) == AEC_OK;
            in_pos += in_size - strm.avail_in;
            out_pos += out_size - strm.avail_out;
            if (in_size == 0 && strm.avail_out == out_size)
                break; // No input left and no progress
        }
        aec_decode_end(&strm);
        return ok && out == expected;
    }

    // Writes bits MSB first
    struct BitWriter
    {
        std::vector<uint8_t> bytes;
        size_t bit = 0;

        void put(uint32_t value, int count)
        {
            for (int i = count - 1; i >= 0; i--, bit++)
            {
                if (bytes.size() <= bit / 8)
                    bytes.push_back(0);
                if ((value >> i) & 1)
                    bytes[bit / 8] |= 0x80 >> (bit % 8);
            }
        }
    };

    // The input padded with zeros to its size, followed by a guard of 0xFF bytes that the decoder
    // is not given. Before #49 was fixed, the decoder read on into the guard (wrapping avail_in)
    // instead of past the end of the buffer.
    std::vector<uint8_t> withGuard(std::vector<uint8_t> in, size_t size, size_t guard)
    {
        in.resize(size, 0);
        in.resize(size + guard, 0xFF);
        return in;
    }
} // namespace

TEST_CASE("aec_decode decodes what aec_encode makes, for every block and sample size", "[libs][aec]")
{
    unsigned int block_size = GENERATE(8, 16, 32, 64);
    test_helpers::Lcg rng(block_size);

    int cases = 0, failed = 0, failed_in_pieces = 0;
    for (unsigned int bits = 1; bits <= 32; bits++)
    {
        std::vector<unsigned int> flag_sets = {0, AEC_DATA_MSB, AEC_DATA_PREPROCESS, AEC_DATA_MSB | AEC_DATA_PREPROCESS};
        if (bits > 16 && bits <= 24)
            flag_sets.push_back(AEC_DATA_3BYTE | AEC_DATA_MSB | AEC_DATA_PREPROCESS);
        if (bits <= 4)
            flag_sets.push_back(AEC_RESTRICTED | AEC_DATA_PREPROCESS);

        for (unsigned int flags : flag_sets)
            for (int kind = 0; kind < 4; kind++)
            {
                AecConfig c = {bits, block_size, 1 + (unsigned int)rng.below(70), flags};
                std::vector<uint8_t> samples = makeSamples(c, 500 + rng.below(300), kind, rng);
                std::vector<uint8_t> encoded = encode(c, samples);
                cases++;
                if (!decodesTo(c, encoded, samples))
                    failed++;
                if (!decodesInPiecesTo(c, encoded, samples, rng))
                    failed_in_pieces++;
            }
    }
    CHECK(cases == (32 * 4 + 8 + 4) * 4);
    CHECK(failed == 0);
    CHECK(failed_in_pieces == 0);
}

TEST_CASE("aec_decode decodes what aec_encode makes of signed samples, for every block and sample size", "[libs][aec]")
{
    // #83: with 1 bit samples xmax was computed with a shift by 32, and the signed pre- and
    // postprocessing overflowed int32_t for 32 bit samples. Fixed by updating libaec (#285).
    unsigned int block_size = GENERATE(8, 16, 32, 64);
    test_helpers::Lcg rng(block_size + 100);

    int cases = 0, failed = 0, failed_in_pieces = 0;
    for (unsigned int bits = 1; bits <= 32; bits++)
        for (unsigned int flags : {0u, (unsigned int)AEC_DATA_MSB, (unsigned int)AEC_DATA_PREPROCESS, (unsigned int)(AEC_DATA_MSB | AEC_DATA_PREPROCESS)})
            for (int kind = 0; kind < 4; kind++)
            {
                AecConfig c = {bits, block_size, 1 + (unsigned int)rng.below(70), flags | AEC_DATA_SIGNED};
                std::vector<uint8_t> samples = makeSamples(c, 500 + rng.below(300), kind, rng);
                std::vector<uint8_t> encoded = encode(c, samples);
                // Without preprocessing the samples are only coded as bits
                std::vector<uint8_t> expected = (flags & AEC_DATA_PREPROCESS) ? signExtended(c, samples) : samples;
                cases++;
                if (!decodesTo(c, encoded, expected))
                    failed++;
                if (!decodesInPiecesTo(c, encoded, expected, rng))
                    failed_in_pieces++;
            }
    CHECK(cases == 32 * 4 * 4);
    CHECK(failed == 0);
    CHECK(failed_in_pieces == 0);
}

TEST_CASE("SZ_BufftoBuffDecompress decodes scanlines longer than 4096 pixels", "[libs][aec]")
{
    // Upstream libaec limits SZ scanlines to 4096 pixels. SatDump does not: GOES HRIT full disk
    // lines have 5424 pixels (8 bit) and Landsat TIRS lines 6000 (16 bit, 16 pixels per block).
    auto settings = GENERATE(std::make_tuple(8, 32, 5424), std::make_tuple(8, 16, 5424), std::make_tuple(16, 16, 6000));
    SZ_com_t p = lineSettings(std::get<1>(settings), std::get<2>(settings));
    p.bits_per_pixel = std::get<0>(settings);

    size_t bytes = (size_t)std::get<2>(settings) * (p.bits_per_pixel / 8) * 3; // 3 scanlines
    std::vector<uint8_t> lines(bytes);
    for (size_t i = 0; i < lines.size(); i++)
        lines[i] = (i / 64) * 3 + (i % 5);

    std::vector<uint8_t> compressed(lines.size() * 2 + 64);
    size_t compressed_size = compressed.size();
    REQUIRE(SZ_BufftoBuffCompress(compressed.data(), &compressed_size, lines.data(), lines.size(), &p) == SZ_OK);

    std::vector<uint8_t> out(lines.size());
    size_t out_size = out.size();
    REQUIRE(SZ_BufftoBuffDecompress(out.data(), &out_size, compressed.data(), compressed_size, &p) == SZ_OK);
    CHECK(out_size == lines.size());
    CHECK(test_helpers::bytesDiff(out, lines) == "");
}

TEST_CASE("SZ_BufftoBuffDecompress only reports the decoded part of a line cut short", "[libs][aec]")
{
    // Before libaec 1.1.8, a scanline that is not a whole number of blocks (5424 pixels with 32
    // per block, as GOES-R sends them) was always reported in full, with what was not decoded
    // taken from uninitialized memory
    int pixels_per_block = GENERATE(16, 32);
    SZ_com_t p = lineSettings(pixels_per_block, 5424);
    std::vector<uint8_t> line(5424);
    for (size_t i = 0; i < line.size(); i++)
        line[i] = 20 + (i * 7) % 200;

    std::vector<uint8_t> compressed(line.size() * 2 + 64);
    size_t compressed_size = compressed.size();
    REQUIRE(SZ_BufftoBuffCompress(compressed.data(), &compressed_size, line.data(), line.size(), &p) == SZ_OK);

    std::vector<uint8_t> out(line.size());
    size_t out_size = out.size();
    REQUIRE(SZ_BufftoBuffDecompress(out.data(), &out_size, compressed.data(), compressed_size / 2, &p) == SZ_OK);
    CHECK(out_size > 0);
    CHECK(out_size < line.size());
    CHECK(std::equal(out.begin(), out.begin() + out_size, line.begin()));
}

TEST_CASE("aec_decode reports split blocks whose fundamental sequences use up the input", "[libs][aec]")
{
    // #49: the fast path only checked once per block that the input held the longest valid block.
    // The run of zeros of a corrupt fundamental sequence is longer, and the decoder read on after
    // its input.
    BitWriter w;
    w.put(2, 3); // Option ID of a split block with k = 1, for 8 bit samples
    for (int i = 0; i < 7; i++)
        w.put(1, 1); // Fundamental sequences of 0
    // Long enough for the fast path: 24 bytes after the 7 that libaec 1.1.8 reads with the option
    // ID. The last fundamental sequence runs to the end of the input, or ends on its last bit and
    // leaves nothing for the binary part.
    const size_t size = 32;
    bool ends_on_last_bit = GENERATE(false, true);
    std::vector<uint8_t> in = withGuard(w.bytes, size, 64);
    if (ends_on_last_bit)
        in[size - 1] = 1;

    std::vector<uint8_t> out(8);
    aec_stream strm = makeStream({8, 8, 1, 0});
    strm.next_in = in.data();
    strm.avail_in = size;
    strm.next_out = out.data();
    strm.avail_out = out.size();
    REQUIRE(aec_decode_init(&strm) == AEC_OK);
    CHECK(aec_decode(&strm, AEC_FLUSH) == AEC_DATA_ERROR);
    CHECK(strm.avail_in <= size);
    // It stays in error, rather than going on from a part of a block
    CHECK(aec_decode(&strm, AEC_FLUSH) == AEC_DATA_ERROR);
    aec_decode_end(&strm);
}

TEST_CASE("aec_decode does not read after the input of a corrupt GOES HRIT line", "[libs][aec]")
{
    // #49: 128 bytes of one byte followed by zeros, for each value of that byte, with the settings
    // of the GOES HRIT decoder. The decoder read after the input for 208 of the 256 values. 128
    // bytes is enough for the fast path with every block size (80 bytes with 64 pixels per block).
    unsigned int pixels_per_block = GENERATE(8, 16, 32, 64);
    AecConfig c = {8, pixels_per_block, (5424 + pixels_per_block - 1) / pixels_per_block, AEC_DATA_MSB | AEC_DATA_PREPROCESS};
    const size_t size = 128;

    int read_after_input = 0, errors = 0;
    for (int first = 0; first < 256; first++)
    {
        // Longer than what the decoder read from it to fill the line
        std::vector<uint8_t> in = withGuard({(uint8_t)first}, size, 8192);
        std::vector<uint8_t> out(c.rsi * pixels_per_block);
        aec_stream strm = makeStream(c);
        strm.next_in = in.data();
        strm.avail_in = size;
        strm.next_out = out.data();
        strm.avail_out = out.size();
        REQUIRE(aec_decode_init(&strm) == AEC_OK);
        int status = aec_decode(&strm, AEC_FLUSH);
        if (strm.avail_in > size) // Wrapped around
            read_after_input++;
        if (status == AEC_DATA_ERROR)
            errors++;
        aec_decode_end(&strm);
    }
    CHECK(read_after_input == 0);
    // The others end without error, as input that runs out does
    CHECK(errors == 160);
}

TEST_CASE("aec_decode reports second extension blocks beyond the end of its table", "[libs][aec]")
{
    // A fundamental sequence above 90 has no entry in the second extension table. The check in
    // the fast path compared against the size of the table in bytes and the slow path had none,
    // so before #49 was fixed such a block read past the decoder state (not tested on master).
    unsigned int fs = GENERATE(90, 91, 200);
    bool fast_path = GENERATE(true, false);

    BitWriter w;
    w.put(0, 3); // Option ID of a low entropy block, for 8 bit samples
    w.put(1, 1); // Second extension
    for (unsigned int i = 0; i < fs; i++)
        w.put(0, 1);
    w.put(1, 1);
    // The fast path needs (32 * 8 + 3) / 8 + 16 = 48 bytes of input
    std::vector<uint8_t> in = w.bytes;
    if (fast_path)
        in.resize(64, 0);
    REQUIRE((in.size() >= 48) == fast_path);

    std::vector<uint8_t> out(32);
    aec_stream strm = makeStream({8, 32, 1, 0});
    strm.next_in = in.data();
    strm.avail_in = in.size();
    strm.next_out = out.data();
    strm.avail_out = out.size();
    REQUIRE(aec_decode_init(&strm) == AEC_OK);
    // 90 is the last entry of the table: the rest of the block is then decoded from the zeros, or
    // ends with the input
    CHECK(aec_decode(&strm, AEC_FLUSH) == (fs > 90 ? AEC_DATA_ERROR : AEC_OK));
    aec_decode_end(&strm);
}
