// Parameters set from another thread (eg, the UI) while the blocks run.
// These mostly matter under ThreadSanitizer / AddressSanitizer : without them,
// they check that the blocks keep running and never overrun an output buffer.
//
// The inputs are chosen so no block frees an empty output buffer from its own
// thread, as that makes two threads produce into one queue (#248).

#include "dsp/agc/agc.h"
#include "dsp/agc/agc_fast.h"
#include "dsp/clock_recovery/clock_recovery_gardner.h"
#include "dsp/clock_recovery/clock_recovery_mm.h"
#include "dsp/clock_recovery/clock_recovery_mm_fast.h"
#include "dsp/digital/cadu_deframer.h"
#include "dsp/filter/decimating_fir.h"
#include "dsp/filter/fft.h"
#include "dsp/filter/fir.h"
#include "dsp/ndsp_test_helpers.h"
#include "dsp/utils/exponentiate.h"
#include "dsp/utils/freq_shift.h"
#include "dsp/utils/hilbert.h"
#include "dsp/utils/throttle.h"
#include <catch2/catch_test_macros.hpp>

using namespace satdump::ndsp;

namespace
{
    /**
     * Runs blk on nbufs input buffers (of min_size to min_size + 1999 samples) while
     * another thread keeps calling change(blk, i) and reading every parameter back.
     * Integer samples are all 0
     */
    template <typename Ti, typename To, typename B>
    void runWithLiveChanges(std::shared_ptr<B> blk, BlockIOType in_type, std::function<void(B &, int)> change, int nbufs = 400, int min_size = 1)
    {
        auto in = std::make_shared<DSPStream>(4);
        blk->set_input({"in", in_type, in}, 0);
        ndsp_test::Collector<To> col(blk->get_output(0, 4).fifo);
        blk->start();

        std::atomic<bool> done = false;
        std::atomic<int> errors = 0;
        // Catch2 assertions are not thread safe : the other thread only counts
        std::thread changer(
            [&]()
            {
                for (int i = 0; !done; i++)
                {
                    try
                    {
                        change(*blk, i);
                        ((Block &)*blk).get_cfg();
                    }
                    catch (std::exception &)
                    {
                        errors++;
                    }
                    std::this_thread::yield();
                }
            });

        ndsp_test::Lcg rng(7);
        for (int i = 0; i < nbufs; i++)
        {
            std::vector<Ti> buf(min_size + (i * 37) % 2000);
            for (auto &v : buf)
            {
                if constexpr (std::is_same_v<Ti, complex_t>)
                    v = complex_t(rng.symmetric(), rng.symmetric());
                else if constexpr (std::is_same_v<Ti, float>)
                    v = rng.symmetric();
                else
                    v = 0;
            }
            ndsp_test::pushSamples(*in, buf);
        }
        ndsp_test::pushTerminator(*in);
        CHECK(ndsp_test::stopBlock(blk));
        done = true;
        changer.join();
        col.finish();

        CHECK(errors == 0);
        CHECK(col.terminators() == 1);
        auto sizes = col.sizes();
        auto max_sizes = col.maxSizes();
        for (size_t i = 0; i < sizes.size(); i++)
            REQUIRE(sizes[i] <= max_sizes[i]);
    }

    std::vector<float> taps(int n, float v = 0.1f) { return std::vector<float>(n, v); }

    // Up to 300 taps : longer inputs always give the FIR blocks some output
    const int fir_min_size = 400;
} // namespace

TEST_CASE("ndsp FIRBlock survives taps changes from another thread while running", "[dsp][filter][live]")
{
    auto b = std::make_shared<FIRBlock<complex_t>>();
    b->set_cfg("taps", taps(31));
    runWithLiveChanges<complex_t, complex_t, FIRBlock<complex_t>>(b, DSP_SAMPLE_TYPE_CF32, [](auto &b, int i) { b.set_cfg("taps", taps(1 + (i * 13) % 300)); }, 400, fir_min_size);

    auto f = std::make_shared<FIRBlock<float>>();
    f->set_cfg("taps", taps(31));
    runWithLiveChanges<float, float, FIRBlock<float>>(f, DSP_SAMPLE_TYPE_F32, [](auto &b, int i) { b.set_cfg("taps", taps(1 + (i * 13) % 300)); }, 400, fir_min_size);
}

TEST_CASE("ndsp DecimatingFIRBlock survives taps and decimation changes from another thread while running", "[dsp][filter][live]")
{
    auto b = std::make_shared<DecimatingFIRBlock<complex_t>>();
    b->set_cfg("taps", taps(31));
    b->set_cfg("decimation", 4);
    runWithLiveChanges<complex_t, complex_t, DecimatingFIRBlock<complex_t>>(b, DSP_SAMPLE_TYPE_CF32,
                                                                           [](auto &b, int i)
                                                                           {
                                                                               b.set_cfg("taps", taps(1 + (i * 13) % 300));
                                                                               // Not 1, which outputs nothing after the first sample (see the shouldfail tests in test_fir.cpp)
                                                                               b.set_cfg("decimation", 2 + i % 7);
                                                                           },
                                                                           400, fir_min_size);
}

TEST_CASE("ndsp FFTFilterBlock survives taps changes from another thread while running", "[dsp][filter][live]")
{
    auto b = std::make_shared<FFTFilterBlock<complex_t>>();
    b->set_cfg("taps", taps(31));
    runWithLiveChanges<complex_t, complex_t, FFTFilterBlock<complex_t>>(b, DSP_SAMPLE_TYPE_CF32, [](auto &b, int i) { b.set_cfg("taps", taps(2 + (i * 13) % 100)); }, 100);
}

TEST_CASE("ndsp AGC blocks survive parameter changes from another thread while running", "[dsp][agc][live]")
{
    auto change = [](auto &b, int i)
    {
        b.set_cfg("rate", 1e-4 * (1 + i % 5));
        b.set_cfg("reference", 0.5 + (i % 3));
        b.set_cfg("gain", 1.0 + (i % 2));
        b.set_cfg("max_gain", 1000.0 * (1 + i % 4));
    };
    runWithLiveChanges<complex_t, complex_t, AGCBlock<complex_t>>(std::make_shared<AGCBlock<complex_t>>(), DSP_SAMPLE_TYPE_CF32, change);
    runWithLiveChanges<complex_t, complex_t, AGCFastBlock<complex_t>>(std::make_shared<AGCFastBlock<complex_t>>(), DSP_SAMPLE_TYPE_CF32, change);
}

TEST_CASE("ndsp clock recovery blocks survive parameter changes from another thread while running", "[dsp][clock_recovery][live]")
{
    auto change = [](auto &b, int i)
    {
        b.set_cfg("omega", 2.0 + (i % 3));
        b.set_cfg("mu", 0.5);
        b.set_cfg("omegaGain", 1e-4 * (1 + i % 3));
        b.set_cfg("muGain", 1e-3 * (1 + i % 3));
        b.set_cfg("omegaLimit", 0.005);
    };
    auto change_pfb = [change](auto &b, int i)
    {
        change(b, i);
        b.set_cfg("nfilt", 32 << (i % 3));
        b.set_cfg("ntaps", 4 + 4 * (i % 3));
    };

    auto mm = std::make_shared<MMClockRecoveryBlock<complex_t>>();
    mm->set_cfg("omega", 2.0);
    runWithLiveChanges<complex_t, complex_t, MMClockRecoveryBlock<complex_t>>(mm, DSP_SAMPLE_TYPE_CF32, change_pfb, 200);

    auto mmf = std::make_shared<MMClockRecoveryFastBlock<complex_t>>();
    mmf->set_cfg("omega", 2.0);
    runWithLiveChanges<complex_t, complex_t, MMClockRecoveryFastBlock<complex_t>>(mmf, DSP_SAMPLE_TYPE_CF32, change, 200);

    auto g = std::make_shared<GardnerClockRecoveryBlock<complex_t>>();
    g->set_cfg("omega", 2.0);
    runWithLiveChanges<complex_t, complex_t, GardnerClockRecoveryBlock<complex_t>>(g, DSP_SAMPLE_TYPE_CF32, change_pfb, 200);
}

TEST_CASE("ndsp utility blocks survive parameter changes from another thread while running", "[dsp][utils][live]")
{
    runWithLiveChanges<float, complex_t, HilbertBlock>(std::make_shared<HilbertBlock>(), DSP_SAMPLE_TYPE_F32, [](auto &b, int i) { b.set_cfg("ntaps", 3 + 2 * (i % 64)); });

    runWithLiveChanges<complex_t, complex_t, FreqShiftBlock>(std::make_shared<FreqShiftBlock>(), DSP_SAMPLE_TYPE_CF32,
                                                             [](auto &b, int i)
                                                             {
                                                                 b.set_cfg("raw_shift", i % 2 == 0);
                                                                 b.set_cfg("freq_shift", 1e3 * (i % 10));
                                                                 b.set_cfg("samplerate", 1e6 * (1 + i % 3));
                                                             });

    runWithLiveChanges<complex_t, complex_t, ExponentiateBlock>(std::make_shared<ExponentiateBlock>(), DSP_SAMPLE_TYPE_CF32, [](auto &b, int i) { b.set_cfg("expo", 2 + i % 6); });

    // Fast enough to not slow the test down
    auto t = std::make_shared<ThrottleBlock<complex_t>>();
    t->set_cfg("samplerate", 1e9);
    runWithLiveChanges<complex_t, complex_t, ThrottleBlock<complex_t>>(t, DSP_SAMPLE_TYPE_CF32, [](auto &b, int i) { b.set_cfg("samplerate", 1e9 * (1 + i % 3)); });
}

TEST_CASE("ndsp CADUDeframerBlock survives CADU size changes from another thread while running", "[dsp][digital][live]")
{
    // Zeros, so it never finds a frame and never outputs anything but the terminator
    runWithLiveChanges<int8_t, int8_t, CADUDeframerBlock>(std::make_shared<CADUDeframerBlock>(), DSP_SAMPLE_TYPE_S8, [](auto &b, int i) { b.set_cfg("cadu_size", 8 * (128 << (i % 4))); });
}
