#include "goes_support/goes/gvar/gvar_headers.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

// get_header() is not declared in a header, it is only used by the image decoder
namespace goes
{
    namespace gvar
    {
        PrimaryBlockHeader get_header(uint8_t *frame);
    }
} // namespace goes

namespace
{
    // A frame with the 3 copies of the 30 bytes header at their offsets
    std::vector<uint8_t> frameWithHeaders(const uint8_t *a, const uint8_t *b, const uint8_t *c)
    {
        std::vector<uint8_t> frame(32786, 0);
        memcpy(&frame[8], a, 30);
        memcpy(&frame[38], b, 30);
        memcpy(&frame[68], c, 30);
        return frame;
    }

    std::vector<uint8_t> headerBytes(const PrimaryBlockHeader &h)
    {
        std::vector<uint8_t> bytes(sizeof(h));
        memcpy(bytes.data(), &h, sizeof(h));
        return bytes;
    }
} // namespace

TEST_CASE("GVAR get_header() decodes the header by bitwise majority of its 3 copies", "[plugins][goes_support][gvar_image_decoder]")
{
    uint8_t good[30];
    for (int i = 0; i < 30; i++)
        good[i] = (uint8_t)(i * 37 + 5);
    good[0] = 0x0A; // The top 4 bits of the first byte are never set

    // Each copy has different bits flipped, so that every byte has at most one bad copy
    uint8_t b[30], c[30];
    memcpy(b, good, 30);
    memcpy(c, good, 30);
    for (int i = 0; i < 30; i++)
    {
        if (i % 2)
            b[i] ^= 0x81;
        else
            c[i] ^= 0x3C;
    }

    auto frame = frameWithHeaders(good, b, c);
    PrimaryBlockHeader h = goes::gvar::get_header(frame.data());

    CHECK(headerBytes(h) == std::vector<uint8_t>(good, good + 30));
    CHECK(h.block_id == 0x0A);
}

TEST_CASE("GVAR get_header() masks the top 4 bits of the block ID", "[plugins][goes_support][gvar_image_decoder]")
{
    uint8_t a[30] = {};
    a[0] = 0xF3;
    auto frame = frameWithHeaders(a, a, a);
    CHECK(goes::gvar::get_header(frame.data()).block_id == 0x03);
}
