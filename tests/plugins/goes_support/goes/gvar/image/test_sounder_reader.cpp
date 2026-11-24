#include "goes_support/goes/gvar/image/sounder_reader.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace
{
    // A sounder block with every position 0, which pushFrame() skips
    std::vector<uint8_t> emptySounderFrame() { return std::vector<uint8_t>(32786, 0); }

    // Number of non-zero pixels of a channel
    size_t nonZero(goes::gvar::SounderReader &r, int ch)
    {
        auto img = r.getImage(ch);
        size_t n = 0;
        for (size_t i = 0; i < img.size(); i++)
            if (img.get(i) != 0)
                n++;
        return n;
    }
} // namespace

TEST_CASE("SounderReader counts the frames pushed since the last clear()", "[plugins][goes_support][gvar_sounder_reader]")
{
    goes::gvar::SounderReader r;
    CHECK(r.frames == 0);

    auto f = emptySounderFrame();
    for (int i = 0; i < 3; i++)
        r.pushFrame(f.data(), 0);
    CHECK(r.frames == 3);

    r.clear();
    CHECK(r.frames == 0);
    r.pushFrame(f.data(), 0);
    CHECK(r.frames == 1);
}

TEST_CASE("SounderReader starts with empty channels and clear() empties them", "[plugins][goes_support][gvar_sounder_reader]")
{
    goes::gvar::SounderReader r;
    for (int ch = 0; ch < 19; ch++)
        CHECK(nonZero(r, ch) == 0);

    // One detector at line 1, pixel 1 of every channel
    auto f = emptySounderFrame();
    f[6113] = 1;
    f[6121] = 1;
    for (int ch = 0; ch < 19; ch++)
        f[6466 + ch * 11 * 4 * 2 + 1] = ch + 1;
    r.pushFrame(f.data(), 0);
    for (int ch = 0; ch < 19; ch++)
    {
        CHECK(nonZero(r, ch) == 1);
        CHECK(r.getImage(ch).get(0) == ch + 1);
    }

    r.clear();
    for (int ch = 0; ch < 19; ch++)
        CHECK(nonZero(r, ch) == 0);
}
