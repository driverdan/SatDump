#include "dsp/block_helpers.h"
#include "dsp/ndsp_test_helpers.h"
#include "dsp/utils/psk_snr_estimator.h"
#include "dsp/utils/reshape_buffer.h"
#include "dsp/utils/samplerate_meter.h"
#include "dsp/utils/throttle.h"
#include "dsp/utils/zero_fill.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using namespace satdump::ndsp;

///////////////////////////////////////////////////////////////////////////////
// PSK SNR estimator
///////////////////////////////////////////////////////////////////////////////

namespace
{
    std::vector<complex_t> noisyBpsk(size_t n, double sigma, uint32_t seed)
    {
        ndsp_test::Lcg rng(seed);
        std::vector<complex_t> v(n);
        for (auto &x : v)
        {
            float s = rng.sign();
            float ni = sigma * rng.gauss();
            float nq = sigma * rng.gauss();
            x = complex_t(s + ni, nq);
        }
        return v;
    }
} // namespace

TEST_CASE("ndsp PSKSnrEstimatorBlock estimates the SNR of noisy BPSK", "[dsp][utils][psk_snr_estimator]")
{
    // Signal power 1, noise power 2 * sigma^2
    for (double sigma : {0.05, 0.1, 0.2})
    {
        INFO("sigma = " << sigma);
        double snr_db = 10 * log10(1.0 / (2 * sigma * sigma));
        auto b = std::make_shared<PSKSnrEstimatorBlock>();
        auto r = ndsp_test::runSimple<complex_t, float>(b, noisyBpsk(40000, sigma, 31), 4096);
        REQUIRE(r.completed);
        CHECK(r.outputs.empty());

        // get_cfg() reads snr first, which also updates signal / noise
        nlohmann::json st = ((Block &)*b).get_cfg();
        CHECK_THAT(st["snr"].get<float>(), WithinAbs(snr_db, 1.5));
        CHECK(std::fabs(st["signal"].get<float>()) < 0.5);
        CHECK_THAT(st["noise"].get<float>(), WithinAbs(10 * log10(2 * sigma * sigma), 1.5));
    }
}

TEST_CASE("ndsp PSKSnrEstimatorBlock lower noise gives a higher SNR", "[dsp][utils][psk_snr_estimator]")
{
    auto run = [](double sigma)
    {
        auto b = std::make_shared<PSKSnrEstimatorBlock>();
        auto r = ndsp_test::runSimple<complex_t, float>(b, noisyBpsk(30000, sigma, 32), 2048);
        REQUIRE(r.completed);
        return b->get_cfg("snr").get<float>();
    };
    float a = run(0.3), c = run(0.1);
    CHECK(c > a + 5);
}

TEST_CASE("ndsp PSKSnrEstimatorBlock id, IO and config", "[dsp][utils][psk_snr_estimator]")
{
    PSKSnrEstimatorBlock b;
    CHECK(b.d_id == "psk_snr_estimator_c");
    CHECK(b.get_inputs().size() == 1);
    CHECK(b.get_outputs().empty());
    CHECK_FALSE(b.is_async());
    auto list = b.get_cfg_list();
    CHECK(list["snr"]["type"] == "stat");
    CHECK(list["noise"]["type"] == "stat");
    CHECK(list["signal"]["type"] == "stat");
    // Stats are read-only, setting them is accepted and ignored
    CHECK(b.set_cfg("snr", 99) == Block::RES_OK);
    CHECK(b.set_cfg("noise", 99) == Block::RES_OK);
    CHECK(b.set_cfg("signal", 99) == Block::RES_OK);
    CHECK_THROWS(b.set_cfg("x", 1));
    CHECK_THROWS(b.get_cfg("x"));
}

///////////////////////////////////////////////////////////////////////////////
// Reshape buffer
///////////////////////////////////////////////////////////////////////////////

namespace
{
    // Feeds the input in chunks, immediately followed by a terminator, without
    // waiting for the output first : every full buffer must still come out, and
    // before the terminator (see #159). With a propagating terminator, only
    // what came out before it is returned.
    template <typename T>
    ndsp_test::RunResult<T> runReshape(std::shared_ptr<ReshapeBufferBlock<T>> b, int bufsize, const std::vector<T> &in, size_t chunk, int terminator = 1)
    {
        b->set_cfg("buffer_size", bufsize);
        ndsp_test::RunOptions opt;
        opt.chunk = chunk;
        opt.terminator = terminator;
        opt.until_output_terminator = terminator == 1;
        return ndsp_test::runBlock<T, T>(b, {in}, opt);
    }

    template <typename T>
    ndsp_test::RunResult<T> runReshape(int bufsize, const std::vector<T> &in, size_t chunk, int terminator = 1)
    {
        return runReshape<T>(std::make_shared<ReshapeBufferBlock<T>>(), bufsize, in, chunk, terminator);
    }

    // Exactly one propagating terminator
    template <typename T>
    void checkTerminatedOnce(ndsp_test::RunResult<T> &r)
    {
        REQUIRE(r.completed);
        CHECK(r.outputs.at(0).terminators == 1);
        CHECK(r.outputs.at(0).propagating_terminators == 1);
    }
} // namespace

TEST_CASE("ndsp ReshapeBufferBlock outputs fixed size buffers", "[dsp][utils][reshape_buffer]")
{
    std::vector<float> in(1000);
    for (size_t i = 0; i < in.size(); i++)
        in[i] = (float)i;
    auto r = runReshape<float>(100, in, 37);
    checkTerminatedOnce(r);
    CHECK(r.out() == in);
    CHECK(r.outputs[0].sizes == std::vector<uint32_t>(10, 100));
}

TEST_CASE("ndsp ReshapeBufferBlock merges small buffers into larger ones", "[dsp][utils][reshape_buffer]")
{
    auto in = ndsp_test::randomComplex(2048, 33);
    auto r = runReshape<complex_t>(512, in, 16);
    checkTerminatedOnce(r);
    CHECK(r.outputs[0].sizes == std::vector<uint32_t>(4, 512));
    REQUIRE(r.out().size() == in.size());
    size_t mismatches = 0, first = 0;
    for (size_t i = 0; i < in.size(); i++)
        if (!ndsp_test::sameBits(r.out()[i], in[i]) && mismatches++ == 0)
            first = i;
    INFO("first mismatch at sample " << first);
    REQUIRE(mismatches == 0);
}

TEST_CASE("ndsp ReshapeBufferBlock splits large buffers into smaller ones", "[dsp][utils][reshape_buffer]")
{
    std::vector<float> in(10000);
    for (size_t i = 0; i < in.size(); i++)
        in[i] = (float)i;
    auto r = runReshape<float>(64, in, 4096);
    checkTerminatedOnce(r);
    // 10000 = 156 * 64 + 16, the last 16 are never output
    CHECK(r.outputs[0].sizes == std::vector<uint32_t>(156, 64));
    CHECK(r.out() == std::vector<float>(in.begin(), in.begin() + 156 * 64));
}

TEST_CASE("ndsp ReshapeBufferBlock keeps an incomplete last buffer", "[dsp][utils][reshape_buffer]")
{
    // 250 samples, 100 per buffer : the last 50 are never output
    std::vector<int8_t> in(250);
    for (size_t i = 0; i < in.size(); i++)
        in[i] = (int8_t)(i % 100);
    auto r = runReshape<int8_t>(100, in, 64);
    checkTerminatedOnce(r);
    CHECK(r.outputs[0].sizes == std::vector<uint32_t>{100, 100});
    CHECK(r.out() == std::vector<int8_t>(in.begin(), in.begin() + 200));
}

TEST_CASE("ndsp ReshapeBufferBlock does not forward a non-propagating terminator", "[dsp][utils][reshape_buffer]")
{
    std::vector<float> in(300, 1.0f);
    auto r = runReshape<float>(100, in, 50, 2);
    REQUIRE(r.completed);
    CHECK(r.outputs[0].terminators == 0);
    CHECK(r.outputs[0].sizes == std::vector<uint32_t>(3, 100));
}

TEST_CASE("ndsp ReshapeBufferBlock can be restarted, and does not carry samples over", "[dsp][utils][reshape_buffer]")
{
    // Each run leaves 30 samples behind, which must not show up in the next one
    auto b = std::make_shared<ReshapeBufferBlock<float>>();
    for (int run = 0; run < 3; run++)
    {
        INFO("run " << run);
        std::vector<float> in(230);
        for (size_t i = 0; i < in.size(); i++)
            in[i] = (float)(run * 1000 + i);
        auto r = runReshape<float>(b, 100, in, 23);
        checkTerminatedOnce(r);
        CHECK(r.out() == std::vector<float>(in.begin(), in.begin() + 200));
    }
}

TEST_CASE("ndsp ReshapeBufferBlock start then immediate terminator, repeated", "[dsp][utils][reshape_buffer]")
{
    // Used to crash (the second thread reading the ring buffer before init()),
    // or to lose data / send it after the terminator (two threads producing
    // into the output stream), see #159
    for (int i = 0; i < 200; i++)
    {
        INFO("iteration " << i);
        auto in = ndsp_test::randomComplex(1024, 100 + i);
        auto r = runReshape<complex_t>(256, in, 1024);
        checkTerminatedOnce(r);
        REQUIRE(r.out().size() == in.size());
        REQUIRE(r.outputs[0].sizes == std::vector<uint32_t>(4, 256));
    }
}

TEST_CASE("ndsp ReshapeBufferBlock does not use uninitialized state on start", "[dsp][utils][reshape_buffer]")
{
    // The block is built in memory full of garbage (as reused heap memory can
    // be), so anything start() uses before initializing it shows up. Its
    // second thread used to read the ring buffer before init(), see #159
    for (int i = 0; i < 50; i++)
    {
        INFO("iteration " << i);
        void *mem = ::operator new(sizeof(ReshapeBufferBlock<float>));
        memset(mem, 0x11, sizeof(ReshapeBufferBlock<float>));
        std::shared_ptr<ReshapeBufferBlock<float>> b(new (mem) ReshapeBufferBlock<float>(),
                                                     [](ReshapeBufferBlock<float> *p)
                                                     {
                                                         p->~ReshapeBufferBlock<float>();
                                                         ::operator delete(p);
                                                     });
        std::vector<float> in(500, 2.0f);
        auto r = runReshape<float>(b, 100, in, 100);
        checkTerminatedOnce(r);
        CHECK(r.outputs[0].sizes == std::vector<uint32_t>(5, 100));
    }
}

TEST_CASE("ndsp ReshapeBufferBlock applies a new buffer_size while running", "[dsp][utils][reshape_buffer]")
{
    auto b = std::make_shared<ReshapeBufferBlock<float>>();
    b->set_cfg("buffer_size", 100);
    // A single input buffer, so getting it back (below) tells when the block is done with the first feed
    auto is = std::make_shared<DSPStream>(1);
    b->set_input({"in", getTypeSampleType<float>(), is}, 0);
    ndsp_test::Collector<float> col(b->get_output(0, 4).fifo);
    b->start();
    std::vector<float> in(1000);
    for (size_t i = 0; i < in.size(); i++)
        in[i] = (float)i;
    {
        ndsp_test::Feeder<float> feed(is, std::vector<float>(in.begin(), in.begin() + 250), 50, 0);
        REQUIRE(feed.wait());
    }
    REQUIRE(col.waitFor(200));
    // Wait until the block gives back the only input buffer : it is then done
    // with the first feed, and the last 50 samples were collected under the old
    // size. Else it could see the new size first and put them in a 300 buffer, see #464
    auto idle = std::async(std::launch::async,
                           [is]()
                           {
                               DSPBuffer bf = is->newBufferSamples(0, sizeof(float));
                               is->free(bf);
                           });
    REQUIRE(idle.wait_for(ndsp_test::kTimeout) == std::future_status::ready);
    b->set_cfg("buffer_size", 300);
    {
        ndsp_test::Feeder<float> feed(is, std::vector<float>(in.begin() + 250, in.end()), 50, 1);
        REQUIRE(feed.wait());
    }
    REQUIRE(col.waitForTerminator());
    REQUIRE(ndsp_test::stopBlock(b));
    col.finish();
    // 100, 100, the 50 collected under the old size as one short buffer,
    // then 2 * 300 out of the 750 left, the last 150 are never output
    CHECK(col.sizes() == std::vector<uint32_t>{100, 100, 50, 300, 300});
    CHECK(col.data() == std::vector<float>(in.begin(), in.begin() + 850));
}

TEST_CASE("ndsp ReshapeBufferBlock ids and config", "[dsp][utils][reshape_buffer]")
{
    CHECK(ReshapeBufferBlock<float>().d_id == "reshape_buffer_ff");
    CHECK(ReshapeBufferBlock<complex_t>().d_id == "reshape_buffer_cc");
    CHECK(ReshapeBufferBlock<int8_t>().d_id == "reshape_buffer_hh");
    ReshapeBufferBlock<float> b;
    CHECK(b.get_cfg("buffer_size") == 1024);
    b.set_cfg("buffer_size", 77);
    CHECK(b.get_cfg("buffer_size") == 77);
    CHECK(b.get_cfg_list()["buffer_size"]["type"] == "int");
    CHECK_THROWS(b.set_cfg("buffer_size", 0));
    CHECK_THROWS(b.set_cfg("buffer_size", -5));
    CHECK(b.get_cfg("buffer_size") == 77);
    CHECK_THROWS(b.get_cfg("x"));
    CHECK_THROWS(b.set_cfg("x", 1));
}

///////////////////////////////////////////////////////////////////////////////
// Zero fill
///////////////////////////////////////////////////////////////////////////////

TEST_CASE("ndsp ZeroFillBlock fills gaps with zeros and forwards data in order", "[dsp][utils][zero_fill]")
{
    auto b = std::make_shared<ZeroFillBlock<float>>();
    b->set_cfg("zero_buffer_size", 16);
    auto is = std::make_shared<DSPStream>(4);
    b->set_input({"in", DSP_SAMPLE_TYPE_F32, is}, 0);
    ndsp_test::Collector<float> col(b->get_output(0, 4).fifo);
    b->start();

    // With no input, zero buffers keep coming
    REQUIRE(col.waitFor(64));

    std::vector<float> data(100);
    for (size_t i = 0; i < data.size(); i++)
        data[i] = (float)(i + 1);
    ndsp_test::pushSamples(*is, data);
    REQUIRE(ndsp_test::waitUntil(
        [&]()
        {
            auto s = col.sizes();
            return std::find(s.begin(), s.end(), 100u) != s.end();
        }));
    ndsp_test::pushTerminator(*is);
    REQUIRE(col.waitForTerminator());
    REQUIRE(ndsp_test::stopBlock(b));
    col.finish();

    auto d = col.data();
    auto sizes = col.sizes();
    // Every buffer is either a zero buffer of 16 samples, or the 100 data samples
    size_t pos = 0, data_buffers = 0;
    for (auto s : sizes)
    {
        if (s == 100)
        {
            data_buffers++;
            for (size_t i = 0; i < 100; i++)
                REQUIRE(d[pos + i] == data[i]);
        }
        else
        {
            REQUIRE(s == 16);
            for (size_t i = 0; i < 16; i++)
                REQUIRE(d[pos + i] == 0.0f);
        }
        pos += s;
    }
    CHECK(data_buffers == 1);
    CHECK(col.terminators() == 1);
}

TEST_CASE("ndsp ZeroFillBlock non-propagating terminator stops silently", "[dsp][utils][zero_fill]")
{
    auto b = std::make_shared<ZeroFillBlock<complex_t>>();
    auto is = std::make_shared<DSPStream>(4);
    b->set_input({"in", DSP_SAMPLE_TYPE_CF32, is}, 0);
    ndsp_test::Collector<complex_t> col(b->get_output(0, 4).fifo);
    b->start();
    REQUIRE(col.waitFor(64));
    ndsp_test::pushTerminator(*is, false);
    REQUIRE(ndsp_test::stopBlock(b));
    col.finish();
    CHECK(col.terminators() == 0);
    for (auto s : col.sizes())
        REQUIRE(s == 64);
}

TEST_CASE("ndsp ZeroFillBlock ids and config", "[dsp][utils][zero_fill]")
{
    CHECK(ZeroFillBlock<float>().d_id == "zero_fill_ff");
    CHECK(ZeroFillBlock<complex_t>().d_id == "zero_fill_cc");
    CHECK(ZeroFillBlock<int8_t>().d_id == "zero_fill_hh");
    ZeroFillBlock<float> b;
    CHECK(b.get_cfg("zero_buffer_size") == 64);
    b.set_cfg("zero_buffer_size", 8);
    CHECK(b.get_cfg("zero_buffer_size") == 8);
    CHECK_THROWS(b.get_cfg("x"));
    CHECK_THROWS(b.set_cfg("x", 1));
}

///////////////////////////////////////////////////////////////////////////////
// Throttle
///////////////////////////////////////////////////////////////////////////////

TEST_CASE("ndsp ThrottleBlock limits the sample rate", "[dsp][utils][throttle]")
{
    // 20000 samples at 100 kS/s take at least 0.2 s
    auto in = ndsp_test::randomFloats(20000, 34);
    auto b = std::make_shared<ThrottleBlock<float>>();
    REQUIRE(ndsp_test::configure(*b, {{"samplerate", 100e3}}) == Block::RES_OK);
    auto t0 = std::chrono::steady_clock::now();
    auto r = ndsp_test::runSimple<float, float>(b, in, 1000);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(r.completed);
    CHECK(r.out() == in);
    CHECK(elapsed >= 0.19);
    CHECK(elapsed < 10.0);
}

TEST_CASE("ndsp ThrottleBlock passes complex samples unchanged", "[dsp][utils][throttle]")
{
    auto in = ndsp_test::randomComplex(5000, 35);
    auto b = std::make_shared<ThrottleBlock<complex_t>>();
    b->set_cfg("samplerate", 1e7);
    auto r = ndsp_test::runSimple<complex_t, complex_t>(b, in, 1000);
    REQUIRE(r.completed);
    REQUIRE(r.out().size() == in.size());
    size_t mismatches = 0, first = 0;
    for (size_t i = 0; i < in.size(); i++)
        if (!ndsp_test::sameBits(r.out()[i], in[i]) && mismatches++ == 0)
            first = i;
    INFO("first mismatch at sample " << first);
    REQUIRE(mismatches == 0);
}

TEST_CASE("ndsp ThrottleBlock ids and config", "[dsp][utils][throttle]")
{
    ThrottleBlock<complex_t> c;
    ThrottleBlock<float> f;
    CHECK(c.d_id == "throttle_cc");
    CHECK(f.d_id == "throttle_ff");
    CHECK(f.get_cfg("samplerate") == 1e6);
    f.set_cfg("samplerate", 2.5e6);
    CHECK(f.get_cfg("samplerate") == 2.5e6);
    CHECK(f.get_cfg_list()["samplerate"]["name"] == "Samplerate");
    CHECK_THROWS(f.get_cfg("x"));
    CHECK_THROWS(f.set_cfg("x", 1));
}

///////////////////////////////////////////////////////////////////////////////
// Samplerate meter
///////////////////////////////////////////////////////////////////////////////

TEST_CASE("ndsp SamplerateMeterBlock passes samples through", "[dsp][utils][samplerate_meter]")
{
    auto in = ndsp_test::randomComplex(3000, 36);
    auto b = std::make_shared<SamplerateMeterBlock<complex_t>>();
    auto r = ndsp_test::runSimple<complex_t, complex_t>(b, in, 512);
    REQUIRE(r.completed);
    REQUIRE(r.out().size() == in.size());
    size_t mismatches = 0, first = 0;
    for (size_t i = 0; i < in.size(); i++)
        if (!ndsp_test::sameBits(r.out()[i], in[i]) && mismatches++ == 0)
            first = i;
    INFO("first mismatch at sample " << first);
    REQUIRE(mismatches == 0);
    CHECK(r.outputs[0].propagating_terminators == 1);
}

TEST_CASE("ndsp SamplerateMeterBlock measures the input rate", "[dsp][utils][samplerate_meter]")
{
    // ~20 kS/s for a bit more than 2 s : the first second only sets the reference
    auto b = std::make_shared<SamplerateMeterBlock<float>>();
    auto is = std::make_shared<DSPStream>(4);
    b->set_input({"in", DSP_SAMPLE_TYPE_F32, is}, 0);
    ndsp_test::Collector<float> col(b->get_output(0, 4).fifo);
    b->start();
    std::vector<float> chunk(1000, 1.0f);
    auto t0 = std::chrono::steady_clock::now();
    size_t sent = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2300))
    {
        ndsp_test::pushSamples(*is, chunk);
        sent += chunk.size();
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(50) * (sent / 1000));
    }
    ndsp_test::pushTerminator(*is);
    REQUIRE(col.waitForTerminator());
    REQUIRE(ndsp_test::stopBlock(b));
    col.finish();
    CHECK(col.size() == sent);
    // Wide bounds : a loaded machine can only slow the feeder down
    CHECK(b->measured_samplerate > 5000);
    CHECK(b->measured_samplerate < 30000);
}

TEST_CASE("ndsp SamplerateMeterBlock ids and config", "[dsp][utils][samplerate_meter]")
{
    CHECK(SamplerateMeterBlock<float>().d_id == "samplerate_meter_ff");
    CHECK(SamplerateMeterBlock<complex_t>().d_id == "samplerate_meter_cc");
    SamplerateMeterBlock<float> b;
    CHECK(b.get_cfg_list().empty());
    CHECK_THROWS(b.get_cfg("x"));
    CHECK_THROWS(b.set_cfg("x", 1));
}
