#include "core/exception.h"
#include "helpers/ccsds_tx.h"
#include "helpers/random.h"
#include "pipeline/modules/demod/module_psk_demod.h"
#include "pipeline/pipeline_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <thread>

// The demodulator is given a root raised cosine shaped BPSK baseband made by
// helpers/ccsds_tx.h, and must give back the symbols that were sent. Unless stated otherwise
// it is set up like the "soft" step of the goes_hrit pipeline, on a 2.4 Msps baseband.
//
// Most tests run it the way a live pipeline does, on a DSP stream: everything sent then
// comes out. From a file, the demodulator runs until the end of the file has gone through
// all of its blocks, so everything comes out as well.

using namespace pipeline_test;
using namespace test_helpers;
using Catch::Matchers::WithinAbs;
namespace pl = satdump::pipeline;

namespace
{
    const double SYMBOLRATE = 927e3;
    const int SYMBOLS = 60000;

    // The demodulator reads the configuration when constructed
    struct DemodEnv
    {
        ConfigGuard cfg;
        TempDir tmp{"psk_demod"};

        DemodEnv() { satdump::satdump_cfg.main_cfg["user_interface"]["show_waterfall_demod_fft"]["value"] = false; }
    };

    // Parameters of the "soft" step of goes_hrit (resources/pipelines/GOES.json)
    nlohmann::json hritParams(double samplerate)
    {
        return {{"constellation", "bpsk"}, {"symbolrate", SYMBOLRATE}, {"rrc_alpha", 0.5}, {"pll_bw", 0.02}, {"max_sps", 3}, {"samplerate", samplerate}, {"baseband_format", "cf32"}};
    }

    // With what "satdump legacy live" adds to them
    nlohmann::json liveParams(double samplerate)
    {
        auto params = hritParams(samplerate);
        params["buffer_size"] = dsp::STREAM_BUFFER_SIZE;
        return params;
    }

    std::vector<complex_t> modulate(const std::vector<uint8_t> &symbols, double samplerate, double phase = 0, double freq_hz = 0)
    {
        return bpskModulate(symbols, samplerate / SYMBOLRATE, 0.5, 0.5f, phase, freq_hz / samplerate);
    }

    struct Match
    {
        int errors = -1;       // Symbols of the compared part that differ from what was sent
        bool inverted = false; // The demodulator locked 180 degrees off
        long offset = 0;       // Sent symbols missing (or, if negative, extra ones) before the compared part
    };

    // Compares `count` demodulated symbols from `start` with what was sent. The output is
    // searched for around its expected place, as symbols are lost or made up while the loops
    // lock. The start should be left out for the same reason.
    Match match(const std::vector<int8_t> &soft, const std::vector<uint8_t> &sent, long start, long count)
    {
        Match best;
        const long search = 500;
        if (start < 0 || start + count > (long)soft.size())
            return best;

        for (long offset = -search; offset <= search; offset++)
        {
            if (start + offset < 0 || start + offset + count > (long)sent.size())
                continue;
            long diff = 0;
            for (long i = 0; i < count; i++)
                diff += (soft[start + i] > 0) != (sent[start + offset + i] != 0);
            long errors = std::min(diff, count - diff);
            if (best.errors == -1 || errors < best.errors)
                best = {(int)errors, diff > count / 2, offset};
        }
        return best;
    }

    // Constructs and initializes the demodulator on a DSP stream, without running it
    std::shared_ptr<pl::ProcessingModule> initLive(nlohmann::json params)
    {
        auto m = pl::demod::PSKDemodModule::getInstance("", "out", params);
        m->input_stream = std::make_shared<dsp::stream<complex_t>>();
        m->output_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
        m->setInputType(pl::DATA_DSP_STREAM);
        m->setOutputType(pl::DATA_STREAM);
        m->init();
        return m;
    }

    struct LiveResult
    {
        bool complete = false; // All the symbols came out in time
        std::vector<int8_t> soft;
        nlohmann::json stats; // While still running
        std::string output;
    };

    // Runs the demodulator as a live pipeline does: the baseband comes from a source through
    // a DSP stream, and the soft symbols go to the next module through a FIFO. The baseband
    // is followed by silence, to push its end through the blocks.
    LiveResult demodLive(std::vector<complex_t> baseband, nlohmann::json params, size_t symbols)
    {
        const size_t block = 4096; // The source writes in blocks, like an RTL-SDR does
        baseband.resize((baseband.size() / block + 40) * block, complex_t(0, 0));

        auto m = initLive(params);
        m->input_active = true;
        std::thread processing([m]() { m->process(); });

        std::thread source(
            [&]()
            {
                for (size_t pos = 0; pos + block <= baseband.size(); pos += block)
                {
                    std::copy(baseband.begin() + pos, baseband.begin() + pos + block, m->input_stream->writeBuf);
                    if (!m->input_stream->swap(block))
                        break;
                }
            });

        LiveResult res;
        res.complete = waitUntil([&]() { return m->output_fifo->getReadable() >= (int)symbols; });
        res.stats = m->getModuleStats();

        // Stopped like LivePipeline::stop() does
        m->input_active = false;
        m->input_stream->stopReader();
        m->input_stream->stopWriter();
        m->stop();
        processing.join();
        source.join();

        res.soft.resize(std::min<size_t>(symbols, m->output_fifo->getReadable()));
        if (!res.soft.empty())
            m->output_fifo->read((uint8_t *)res.soft.data(), res.soft.size());
        res.output = m->getOutput();
        return res;
    }

    // Errors in the second half of what was demodulated, see match()
    Match matchSecondHalf(const LiveResult &res, const std::vector<uint8_t> &sent) { return match(res.soft, sent, res.soft.size() / 2, res.soft.size() / 2 - 1000); }
} // namespace

TEST_CASE("PSKDemodModule identifies itself", "[pipeline][modules][demod][psk_demod]")
{
    DemodEnv env;
    CHECK(pl::demod::PSKDemodModule::getID() == "psk_demod");
    auto m = pl::demod::PSKDemodModule::getInstance("in.cf32", "out", hritParams(2.4e6));
    CHECK(m->getIDM() == "psk_demod");
    CHECK(m->getInputTypes() == std::vector<pl::ModuleDataType>{pl::DATA_FILE, pl::DATA_DSP_STREAM});
    CHECK(m->getOutputTypes() == std::vector<pl::ModuleDataType>{pl::DATA_FILE, pl::DATA_STREAM});

    auto defaults = pl::demod::PSKDemodModule::getParams();
    CHECK(defaults["constellation"] == "bpsk");
    CHECK(defaults.contains("rrc_alpha"));
    CHECK(defaults.contains("pll_bw"));
    CHECK(defaults.contains("samplerate")); // From the base demodulator
}

TEST_CASE("PSKDemodModule requires its mandatory parameters", "[pipeline][modules][demod][psk_demod]")
{
    DemodEnv env;
    auto params = liveParams(2.4e6);

    SECTION("constructor")
    {
        auto key = GENERATE("samplerate", "constellation", "rrc_alpha", "pll_bw");
        INFO(key);
        params.erase(key);
        CHECK_THROWS_AS(pl::demod::PSKDemodModule::getInstance("in.cf32", "out", params), satdump::satdump_exception_t);
    }

    SECTION("a samplerate below the symbolrate is refused by init")
    {
        params["samplerate"] = 900e3;
        CHECK_THROWS_AS(initLive(params), satdump::satdump_exception_t);
    }

    SECTION("carrier mode needs a carrier PLL bandwidth")
    {
        params["has_carrier"] = true;
        CHECK_THROWS_AS(initLive(params), satdump::satdump_exception_t);
    }

    SECTION("carrier mode is BPSK only")
    {
        params["has_carrier"] = true;
        params["carrier_pll_bw"] = 0.01;
        params["constellation"] = "qpsk";
        CHECK_THROWS_AS(initLive(params), satdump::satdump_exception_t);
    }
}

TEST_CASE("PSKDemodModule demodulates a live BPSK stream to soft symbols", "[pipeline][modules][demod][psk_demod]")
{
    DemodEnv env;
    auto sent = randomBits(SYMBOLS, 1);

    double samplerate = 2.4e6, phase = 0, freq_hz = 0;

    SECTION("at 2.4 Msps, 2.59 samples per symbol") {}

    // Not exactly 90 degrees: without noise the Costas loop then has nothing to pull it away
    SECTION("with any carrier phase") { phase = GENERATE(1.0, M_PI, 4.0); }

    SECTION("with a carrier offset, which is tracked and reported") { freq_hz = GENERATE(8e3, -15e3); }

    SECTION("at the 6 Msps of the pipeline preset, resampled to 3 samples per symbol") { samplerate = 6e6; }

    CAPTURE(samplerate, phase, freq_hz);
    auto res = demodLive(modulate(sent, samplerate, phase, freq_hz), liveParams(samplerate), SYMBOLS - 1000);

    REQUIRE(res.complete);
    auto m = matchSecondHalf(res, sent);
    CAPTURE(m.offset, m.inverted);
    CHECK(m.errors == 0);
    CHECK(std::abs(m.offset) < 100); // One soft symbol per symbol sent

    // Symbols are scaled to about +-50, well within an int8
    int weak = 0, clipped = 0;
    for (size_t i = res.soft.size() / 2; i < res.soft.size() - 1000; i++)
    {
        weak += std::abs(res.soft[i]) < 20;
        clipped += std::abs(res.soft[i]) >= 127;
    }
    CHECK(weak == 0);
    CHECK(clipped == 0);

    CHECK(res.stats["peak_snr"].get<float>() > 15.0f); // No noise was added
    CHECK(res.stats["peak_snr"].get<float>() >= res.stats["snr"].get<float>());
    CHECK(res.stats["freq"].is_number());
    CHECK(res.output == ""); // Nothing is written to disk
}

TEST_CASE("PSKDemodModule reports the carrier offset it tracks", "[pipeline][modules][demod][psk_demod]")
{
    DemodEnv env;
    auto sent = randomBits(SYMBOLS, 5);
    double freq_hz = GENERATE(0.0, 8e3, -15e3);
    CAPTURE(freq_hz);

    // Not followed by silence: the statistics are read while the signal is still there
    auto baseband = modulate(sent, 2.4e6, 0, freq_hz);
    auto m = initLive(liveParams(2.4e6));
    m->input_active = true;
    std::thread processing([m]() { m->process(); });

    const size_t block = 4096;
    size_t pos = 0;
    for (; pos + block <= baseband.size() * 3 / 4; pos += block)
    {
        std::copy(baseband.begin() + pos, baseband.begin() + pos + block, m->input_stream->writeBuf);
        m->input_stream->swap(block);
    }
    bool got = waitUntil([&]() { return m->output_fifo->getReadable() >= SYMBOLS / 2; });
    auto stats = m->getModuleStats();

    m->input_active = false;
    m->input_stream->stopReader();
    m->input_stream->stopWriter();
    m->stop();
    processing.join();

    REQUIRE(got);
    CHECK_THAT(stats["freq"].get<float>(), WithinAbs(freq_hz, 1e3));
    CHECK(stats["snr"].get<float>() > 15.0f);
}

TEST_CASE("PSKDemodModule optional blocks do not disturb a clean signal", "[pipeline][modules][demod][psk_demod]")
{
    DemodEnv env;
    auto sent = randomBits(SYMBOLS, 2);
    auto params = liveParams(2.4e6);
    auto baseband = modulate(sent, 2.4e6);

    SECTION("DC blocking")
    {
        for (auto &s : baseband) // An RTL-SDR has a DC spike
            s = s + complex_t(0.05f, -0.03f);
        params["dc_block"] = true;
    }

    SECTION("frequency shift")
    {
        baseband = modulate(sent, 2.4e6, 0, 300e3);
        params["freq_shift"] = -300e3;
    }

    SECTION("DC blocking after the Costas loop") { params["post_costas_dc"] = true; }

    SECTION("clock recovery and filter settings")
    {
        params["clock_alpha"] = 8.7e-3;
        params["clock_gain_omega"] = 1.9e-5;
        params["clock_mu"] = 0.5;
        params["clock_gain_mu"] = 8.7e-3;
        params["clock_omega_relative_limit"] = 0.005;
        params["rrc_taps"] = 31;
        params["agc_rate"] = 1e-2;
        params["min_sps"] = 1.1;
    }

    auto res = demodLive(baseband, params, SYMBOLS - 1000);

    REQUIRE(res.complete);
    auto m = matchSecondHalf(res, sent);
    CAPTURE(m.offset, m.inverted);
    CHECK(m.errors == 0);
}

TEST_CASE("PSKDemodModule demodulates a baseband file", "[pipeline][modules][demod][psk_demod]")
{
    // Long enough for the start of the file to come out before its end is read
    DemodEnv env;
    const int symbols = 400000;
    auto sent = randomBits(symbols, 3);
    std::string input = env.tmp.file("in.cf32");
    auto params = hritParams(2.4e6);
    auto baseband = modulate(sent, 2.4e6);

    SECTION("as is") {}

    SECTION("IQ swapped")
    {
        for (auto &s : baseband)
            s = complex_t(s.imag, s.real);
        params["iq_swap"] = true;
    }

    SECTION("recording the baseband it demodulates") { params["dump_intermediate"] = "cf32"; }

    writeBytes(input, baseband);

    auto m = pl::demod::PSKDemodModule::getInstance(input, env.tmp.file("out"), params);
    m->setInputType(pl::DATA_FILE);
    m->setOutputType(pl::DATA_FILE);
    m->init();
    m->process();

    CHECK(m->getOutput() == env.tmp.file("out") + ".soft");
    auto soft = readBytes<int8_t>(m->getOutput());
    REQUIRE(soft.size() > (size_t)symbols / 4);

    auto res = match(soft, sent, 20000, 50000);
    CAPTURE(res.offset, res.inverted, soft.size());
    CHECK(res.errors == 0);

    // Up to the end of the file
    CHECK(soft.size() > (size_t)symbols - 100);
    CHECK(soft.size() < (size_t)symbols + 100);

    auto stats = m->getModuleStats();
    CHECK(stats["peak_snr"].get<float>() > 15.0f);
    CHECK(stats["progress"].is_number());

    if (params.contains("dump_intermediate"))
    {
        std::string dump = env.tmp.file("out") + "_2400000_intermediate_iq.cf32";
        REQUIRE(std::filesystem::exists(dump));
        CHECK(std::filesystem::file_size(dump) > std::filesystem::file_size(input) / 4);
        // The last buffer of the file is recorded whole
        CHECK(std::filesystem::file_size(dump) <= std::filesystem::file_size(input) + 12000 * sizeof(complex_t));
    }
}

TEST_CASE("PSKDemodModule demodulates a baseband file to its end", "[pipeline][modules][demod][psk_demod]")
{
    // A few buffers of the file source only, which is less than what the blocks of the
    // demodulator hold: none of it came out when the demodulator stopped at the end of the file
    DemodEnv env;
    const int symbols = 20000;
    auto sent = randomBits(symbols, 4);
    std::string input = env.tmp.file("in.cf32");
    auto params = hritParams(2.4e6);

    SECTION("at 2.4 Msps") { writeBytes(input, modulate(sent, 2.4e6)); }

    SECTION("at 6 Msps, resampled")
    {
        params["samplerate"] = 6e6;
        writeBytes(input, modulate(sent, 6e6));
    }

    // The end used to depend on timing, so it is checked several times
    for (int run = 0; run < 3; run++)
    {
        CAPTURE(run);
        std::string output = env.tmp.file("out" + std::to_string(run));
        auto m = pl::demod::PSKDemodModule::getInstance(input, output, params);
        m->setInputType(pl::DATA_FILE);
        m->setOutputType(pl::DATA_FILE);
        m->init();
        m->process();

        auto soft = readBytes<int8_t>(m->getOutput());
        CAPTURE(soft.size());
        REQUIRE(soft.size() > (size_t)symbols - 100);
        CHECK(soft.size() < (size_t)symbols + 100);

        // The last symbols are demodulated too
        auto res = match(soft, sent, soft.size() - 6000, 5000);
        CAPTURE(res.offset, res.inverted);
        CHECK(res.errors == 0);
        CHECK(std::abs(res.offset) < 100);
    }
}
