#include "dvb_support/dvbs/viterbi_all.h"
#include "common/codings/viterbi/cc_encoder.h"
#include "helpers/prefilled.h"
#include "helpers/random.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>
#include <vector>

namespace
{
    const int BUFFER = 8192; // received soft symbols per work() call

    struct Rate
    {
        std::string name;
        std::string x, y; // DVB-S puncturing pattern
    };

    std::vector<int8_t> encodePunctured(const std::vector<uint8_t> &bits, const Rate &rate)
    {
        viterbi::CCEncoder enc(bits.size(), 7, 2, {79, 109});
        std::vector<uint8_t> s(bits.size() * 2);
        enc.work((uint8_t *)bits.data(), s.data());
        std::vector<int8_t> out;
        int period = rate.x.size();
        for (size_t b = 0; b < bits.size(); b++)
        {
            if (rate.x[b % period] == '1')
                out.push_back(s[b * 2] ? 100 : -100);
            if (rate.y[b % period] == '1')
                out.push_back(s[b * 2 + 1] ? 100 : -100);
        }
        return out;
    }
} // namespace

TEST_CASE("Viterbi_DVBS ber() before a lock is the best BER of every tested branch", "[dvb][viterbi]")
{
    // With a negative threshold the decoder never locks, so ber() reports the best branch found by
    // the lock search. Clean data has a branch with next to no errors. The BERs of PHASE_180 and
    // PHASE_270 used to be stored past the end of their arrays, over those of the next rate (#434)
    auto rate = GENERATE(Rate{"1/2", "1", "1"}, Rate{"2/3", "10", "11"}, Rate{"3/4", "101", "110"}, Rate{"5/6", "10101", "11010"},
                         Rate{"7/8", "1000101", "1111010"});
    int offset = GENERATE(0, 2, 4, 6);
    INFO("rate " << rate.name << ", offset " << offset);

    std::vector<uint8_t> bits = test_helpers::randomBits(BUFFER * 2, 5);
    std::vector<int8_t> stream = encodePunctured(bits, rate);
    std::vector<int8_t> in(stream.begin() + offset, stream.begin() + offset + BUFFER);

    // All four phases, the default
    auto vitp = test_helpers::makePrefilled<viterbi::Viterbi_DVBS>(128, -1.0f, 5, BUFFER);
    viterbi::Viterbi_DVBS &vit = *vitp;
    std::vector<uint8_t> out(BUFFER * 8);
    CHECK(vit.work(in.data(), BUFFER, out.data()) == 0);
    CHECK(vit.getState() == 0);
    CHECK(vit.ber() < 0.05f); // Wrong branches are around 0.2 or more
}
