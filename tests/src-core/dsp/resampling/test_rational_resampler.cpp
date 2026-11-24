#include "dsp/ndsp_sig_test_helpers.h"
#include "dsp/ndsp_test_helpers.h"
#include "dsp/resampling/rational_resampler.h"
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <thread>

using Catch::Approx;
using namespace satdump::ndsp;

namespace
{
    template <typename T>
    std::shared_ptr<RationalResamplerBlock<T>> makeResampler(double interp, double decim)
    {
        auto r = std::make_shared<RationalResamplerBlock<T>>();
        REQUIRE(ndsp_sig_test::setAll(*r, {{"interpolation", interp}, {"decimation", decim}}) == Block::RES_OK);
        return r;
    }
} // namespace

TEST_CASE("ndsp RationalResamplerBlock<complex_t> output rate, tone frequency and gain", "[dsp][resampling]")
{
    struct Case
    {
        double interp, decim;
    };
    // Including ratios given as sample rates, reduced by their GCD internally
    for (auto c : {Case{1, 2}, Case{3, 2}, Case{2, 1}, Case{2, 3}, Case{48000, 96000}, Case{48000, 250000}, Case{5, 1}})
    {
        double ratio = c.interp / c.decim;
        INFO("interpolation " << c.interp << " decimation " << c.decim);

        // Tone at 0.05 cycles per output sample, well inside the passband
        const size_t n = 60000;
        double w_out = 2 * M_PI * 0.05;
        double w_in = w_out * ratio;
        auto in = ndsp_sig_test::complexTone(n, w_in, 0.8);

        auto res = makeResampler<complex_t>(c.interp, c.decim);
        auto r = ndsp_sig_test::runBlock<complex_t, complex_t>(res, in, ndsp_sig_test::chunked(4096));
        REQUIRE(r.completed);
        auto &out = r.output();

        // Output count, minus the filter history held back
        double expected = n * ratio;
        CHECK(out.size() <= expected + 1);
        CHECK(out.size() >= expected - 400 * std::max(1.0, ratio));

        size_t skip = out.size() / 10;
        CHECK(ndsp_sig_test::estimateFreq(out, skip, out.size()) == Approx(w_out).epsilon(1e-3));
        CHECK(ndsp_sig_test::toneAmplitude(out, w_out, skip, out.size()) == Approx(0.8).epsilon(0.01));
    }
}

TEST_CASE("ndsp RationalResamplerBlock<float> decimation keeps a DC level and rejects aliases", "[dsp][resampling]")
{
    // DC + a tone that would alias after decimation by 4
    const size_t n = 80000;
    auto in = ndsp_sig_test::realTone(n, 2 * M_PI * 0.3, 1.0);
    for (auto &v : in)
        v += 0.5f;
    auto res = makeResampler<float>(1, 4);
    auto r = ndsp_sig_test::runBlock<float, float>(res, in);
    REQUIRE(r.completed);
    auto &out = r.output();
    REQUIRE(out.size() == Approx(n / 4).margin(100));

    // 0.3 cycles/sample aliases to 0.2 cycles/output sample
    size_t skip = 500;
    double mean = 0;
    for (size_t i = skip; i < out.size(); i++)
        mean += out[i];
    mean /= (double)(out.size() - skip);
    CHECK(mean == Approx(0.5).epsilon(0.01));
    CHECK(ndsp_sig_test::toDb(ndsp_sig_test::toneAmplitude(out, 2 * M_PI * 0.2, skip, out.size())) < -40);
}

TEST_CASE("ndsp RationalResamplerBlock with custom single tap passes samples through", "[dsp][resampling]")
{
    auto res = std::make_shared<RationalResamplerBlock<float>>();
    REQUIRE(ndsp_sig_test::setAll(*res, {{"interpolation", 1}, {"decimation", 1}, {"taps", std::vector<float>{1.0f}}}) == Block::RES_OK);
    std::vector<float> in(10000);
    for (size_t i = 0; i < in.size(); i++)
        in[i] = (float)i;
    auto r = ndsp_sig_test::runBlock<float, float>(res, in, ndsp_sig_test::chunked(333));
    REQUIRE(r.completed);
    auto &out = r.output();
    // One sample is always held back, waiting for the next buffer
    REQUIRE(out.size() == in.size() - 1);
    size_t mismatches = 0, first = 0;
    for (size_t i = 0; i < out.size(); i++)
        if (!(out[i] == in[i]) && mismatches++ == 0)
            first = i;
    INFO("first mismatch at sample " << first);
    REQUIRE(mismatches == 0);
}

TEST_CASE("ndsp RationalResamplerBlock custom taps implement an interpolator", "[dsp][resampling]")
{
    // Interpolation by 2 with taps {1, 1} : zero-order hold
    auto res = std::make_shared<RationalResamplerBlock<float>>();
    REQUIRE(ndsp_sig_test::setAll(*res, {{"interpolation", 2}, {"decimation", 1}, {"taps", std::vector<float>{1.0f, 1.0f}}}) == Block::RES_OK);
    std::vector<float> in = {1, 2, 3, 4, 5, 6};
    auto r = ndsp_sig_test::runBlock<float, float>(res, in);
    REQUIRE(r.completed);
    std::vector<float> expected = {1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
    CHECK(r.output() == expected);
}

namespace
{
    std::vector<float> ramp(float start, size_t n)
    {
        std::vector<float> v(n);
        for (size_t i = 0; i < n; i++)
            v[i] = start + (float)i;
        return v;
    }

    // Each value of [start, start + n) repeated `times` times
    std::vector<float> rampHeld(float start, size_t n, int times = 2)
    {
        std::vector<float> v;
        for (size_t i = 0; i < n; i++)
            v.insert(v.end(), times, start + (float)i);
        return v;
    }

    void append(std::vector<float> &v, const std::vector<float> &o) { v.insert(v.end(), o.begin(), o.end()); }

    // A running float resampler, fed by hand. Checks never stop the test while the block
    // runs : finish() always stops it, so a failure stays local to its test case
    struct LiveResampler
    {
        std::shared_ptr<RationalResamplerBlock<float>> blk = std::make_shared<RationalResamplerBlock<float>>();
        std::shared_ptr<DSPStream> in = std::make_shared<DSPStream>(4);
        std::unique_ptr<ndsp_test::Collector<float>> col;

        LiveResampler(int interp, const std::vector<float> &taps)
        {
            set(interp, taps);
            blk->set_input({"in", DSP_SAMPLE_TYPE_F32, in}, 0);
            col = std::make_unique<ndsp_test::Collector<float>>(blk->get_output(0, 4).fifo);
            blk->start();
        }

        void set(int interp, const std::vector<float> &taps) { CHECK(ndsp_sig_test::setAll(*blk, {{"interpolation", interp}, {"decimation", 1}, {"taps", taps}}) == Block::RES_OK); }
        void push(const std::vector<float> &v) { ndsp_test::pushSamples(*in, v); }
        // Waiting for the output of an input also means it was processed, so a set() after applies to the next inputs
        void waitFor(size_t n) { CHECK(col->waitFor(n)); }

        // Stops the block and checks the output, and that no output buffer was written past its max_size
        void finish(const std::vector<float> &expected)
        {
            ndsp_test::pushTerminator(*in);
            CHECK(ndsp_test::stopBlock(blk));
            col->finish();
            CHECK(col->data().size() == expected.size());
            CHECK(col->data() == expected);
            auto sizes = col->sizes();
            auto max_sizes = col->maxSizes();
            for (size_t i = 0; i < sizes.size(); i++)
                CHECK(sizes[i] <= max_sizes[i]);
        }
    };

    const std::vector<float> taps1 = {1.0f}, taps2 = {1.0f, 1.0f}, taps3 = {1.0f, 1.0f, 1.0f};
} // namespace

// Pass-through holds one sample back. Interpolation by N with N taps of 1 repeats each input N
// times, holding none back. A rate change drops what is in the internal buffer, and on a
// rise the whole first input after it, as its output buffer was sized for the old ratio

TEST_CASE("ndsp RationalResamplerBlock applies a higher rate set while running", "[dsp][resampling]")
{
    LiveResampler r(1, taps1);
    r.push(ramp(0, 100));
    r.waitFor(99);

    r.set(2, taps2);
    r.push(ramp(100, 100)); // Dropped
    r.push(ramp(200, 100));
    r.waitFor(99 + 198);

    auto expected = ramp(0, 99);
    append(expected, rampHeld(200, 99));
    r.finish(expected);
}

TEST_CASE("ndsp RationalResamplerBlock applies a higher rate set while running, then gets a smaller input", "[dsp][resampling]")
{
    LiveResampler r(1, taps1);
    r.push(ramp(0, 100));
    r.waitFor(99);

    r.set(2, taps2);
    r.push(ramp(100, 100)); // Dropped
    r.push(ramp(200, 10));  // Output buffer of 40 samples
    r.push(ramp(210, 100));
    r.waitFor(99 + 18 + 200);

    auto expected = ramp(0, 99);
    append(expected, rampHeld(200, 109));
    r.finish(expected);
}

TEST_CASE("ndsp RationalResamplerBlock applies a lower rate set while running", "[dsp][resampling]")
{
    LiveResampler r(2, taps2);
    r.push(ramp(0, 100));
    r.waitFor(198);

    r.set(1, taps1);
    r.push(ramp(100, 100));
    r.waitFor(198 + 99);

    auto expected = rampHeld(0, 99);
    append(expected, ramp(100, 99));
    r.finish(expected);
}

TEST_CASE("ndsp RationalResamplerBlock applies consecutive rate changes while running", "[dsp][resampling]")
{
    LiveResampler r(1, taps1);
    r.push(ramp(0, 100));
    r.waitFor(99);

    r.set(2, taps2);
    r.push(ramp(100, 100)); // Dropped
    r.push(ramp(200, 100));
    r.waitFor(99 + 198);

    r.set(3, taps3);
    r.push(ramp(300, 100)); // Dropped
    r.push(ramp(400, 100));
    r.waitFor(99 + 198 + 297);

    r.set(1, taps1);
    r.push(ramp(500, 100));
    r.waitFor(99 + 198 + 297 + 99);

    auto expected = ramp(0, 99);
    append(expected, rampHeld(200, 99));
    append(expected, rampHeld(400, 99, 3));
    append(expected, ramp(500, 99));
    r.finish(expected);
}

TEST_CASE("ndsp RationalResamplerBlock applies a higher rate set while running with big buffers", "[dsp][resampling]")
{
    const size_t n = 500000;
    LiveResampler r(1, taps1);
    r.push(ramp(0, n));
    r.waitFor(n - 1);

    r.set(2, taps2);
    r.push(ramp(n, n)); // Dropped
    r.push(ramp(2 * n, n));
    r.push(ramp(3 * n, n));
    r.waitFor((n - 1) + 2 * (n - 1) + 2 * n);

    auto expected = ramp(0, n - 1);
    append(expected, rampHeld(2 * n, 2 * n - 1));
    r.finish(expected);
}

TEST_CASE("ndsp RationalResamplerBlock configuration round-trip", "[dsp][resampling]")
{
    RationalResamplerBlock<complex_t> r;
    CHECK(r.d_id == "rational_resampler_cc");
    CHECK(RationalResamplerBlock<float>().d_id == "rational_resampler_ff");
    auto def = ndsp_sig_test::getAll(r);
    CHECK(def == nlohmann::json({{"interpolation", 1.0}, {"decimation", 2.0}}));

    CHECK(ndsp_sig_test::setAll(r, {{"interpolation", 3}, {"decimation", 7}}) == Block::RES_OK);
    CHECK(ndsp_sig_test::getAll(r) == nlohmann::json({{"interpolation", 3.0}, {"decimation", 7.0}}));
    std::vector<float> t = {0.5f, 0.25f};
    CHECK(r.set_cfg("taps", t) == Block::RES_OK);
    CHECK(r.get_cfg("taps").get<std::vector<float>>() == t);
    CHECK_THROWS(r.set_cfg("x", 1));
    CHECK_THROWS(r.get_cfg("x"));
}

TEST_CASE("ndsp RationalResamplerBlock rejects an interpolation or decimation below 1", "[dsp][resampling]")
{
    RationalResamplerBlock<float> r;
    REQUIRE(ndsp_sig_test::setAll(r, {{"interpolation", 3}, {"decimation", 7}}) == Block::RES_OK);
    // Values init() would truncate to 0 (a division by zero), or that do not fit an int
    for (double bad : {0.0, 0.5, 0.999, -1.0, -48000.0, 3e9, (double)NAN, (double)INFINITY})
    {
        INFO("value " << bad);
        CHECK(r.set_cfg("interpolation", bad) == Block::RES_ERR);
        CHECK(r.set_cfg("decimation", bad) == Block::RES_ERR);
        CHECK(r.set_ratio(bad, 2) == Block::RES_ERR);
        CHECK(r.set_ratio(2, bad, {1.0f}) == Block::RES_ERR);
    }
    // Nothing was applied, not even the valid parts of a rejected set_ratio
    CHECK(ndsp_sig_test::getAll(r) == nlohmann::json({{"interpolation", 3.0}, {"decimation", 7.0}}));
    CHECK(r.get_cfg("taps").get<std::vector<float>>().empty());

    CHECK(r.set_cfg("interpolation", 1) == Block::RES_OK);
    CHECK(r.set_cfg("decimation", 1.5) == Block::RES_OK);
    CHECK(r.set_ratio(5, 2, {0.5f}) == Block::RES_OK);
    CHECK(ndsp_sig_test::getAll(r) == nlohmann::json({{"interpolation", 5.0}, {"decimation", 2.0}}));
    CHECK(r.get_cfg("taps").get<std::vector<float>>() == std::vector<float>{0.5f});
    CHECK(r.set_ratio(4, 3) == Block::RES_OK); // Keeps the taps
    CHECK(ndsp_sig_test::getAll(r) == nlohmann::json({{"interpolation", 4.0}, {"decimation", 3.0}}));
    CHECK(r.get_cfg("taps").get<std::vector<float>>() == std::vector<float>{0.5f});
}

TEST_CASE("ndsp RationalResamplerBlock keeps running at its rate after a rejected change", "[dsp][resampling]")
{
    LiveResampler r(2, taps2);
    r.push(ramp(0, 100));
    r.waitFor(198);

    // These used to make init() divide by zero
    CHECK(r.blk->set_cfg("interpolation", 0) == Block::RES_ERR);
    CHECK(r.blk->set_cfg("decimation", 0.5) == Block::RES_ERR);
    r.push(ramp(100, 100));
    r.waitFor(198 + 200);

    r.finish(rampHeld(0, 199));
}

TEST_CASE("ndsp RationalResamplerBlock applies set_ratio while running", "[dsp][resampling]")
{
    LiveResampler r(1, taps1);
    r.push(ramp(0, 100));
    r.waitFor(99);

    CHECK(r.blk->set_ratio(2, 1, taps2) == Block::RES_OK);
    r.push(ramp(100, 100)); // Dropped
    r.push(ramp(200, 100));
    r.waitFor(99 + 198);

    auto expected = ramp(0, 99);
    append(expected, rampHeld(200, 99));
    r.finish(expected);
}

TEST_CASE("ndsp RationalResamplerBlock survives rate changes from another thread while running", "[dsp][resampling]")
{
    LiveResampler r(1, taps1);
    std::atomic<bool> done = false;
    std::atomic<int> bad_results = 0;
    // Catch2 assertions are not thread safe : the other thread only counts
    std::thread changer(
        [&]()
        {
            for (int i = 0; !done; i++)
            {
                if (r.blk->set_ratio(1 + i % 3, 1 + i % 2, i % 2 ? taps2 : taps1) != Block::RES_OK)
                    bad_results++;
                if (r.blk->set_cfg("interpolation", 0) != Block::RES_ERR)
                    bad_results++;
                std::this_thread::yield();
            }
        });
    for (int i = 0; i < 2000; i++)
        r.push(ramp(0, 1 + i % 50));
    ndsp_test::pushTerminator(*r.in);
    CHECK(ndsp_test::stopBlock(r.blk));
    done = true;
    changer.join();
    r.col->finish();
    CHECK(bad_results == 0);

    // The output depends on the timing, but no output buffer may have been overrun
    auto sizes = r.col->sizes();
    auto max_sizes = r.col->maxSizes();
    for (size_t i = 0; i < sizes.size(); i++)
        CHECK(sizes[i] <= max_sizes[i]);
}
