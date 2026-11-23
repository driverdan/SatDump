#include "core/exception.h"
#include "helpers/ccsds_tx.h"
#include "helpers/compare.h"
#include "helpers/prefilled.h"
#include "helpers/random.h"
#include "pipeline/modules/ccsds/module_ccsds_conv_concat_decoder.h"
#include "pipeline/pipeline_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thread>

// The decoder is given soft symbols of CADUs encoded by helpers/ccsds_tx.h: Reed-Solomon
// (255,223) I=4, randomized, NRZ-M, then the rate 1/2 convolutional code. Unless stated
// otherwise it is set up like the "cadu" step of the goes_hrit pipeline.

using namespace pipeline_test;
using namespace test_helpers;
namespace pl = satdump::pipeline;

namespace
{
    // Parameters of the "cadu" step of goes_hrit (resources/pipelines/GOES.json)
    nlohmann::json hritParams()
    {
        return {{"constellation", "bpsk"}, {"cadu_size", 8192},  {"viterbi_ber_thresold", 0.300}, {"viterbi_outsync_after", 20}, {"derandomize", true}, {"nrzm", true}, {"rs_i", 4},
                {"rs_type", "rs223"},      {"rs_usecheck", true}};
    }

    std::vector<std::vector<uint8_t>> makeCadus(int count, uint32_t seed)
    {
        std::vector<std::vector<uint8_t>> cadus;
        for (int i = 0; i < count; i++)
            cadus.push_back(makeCadu(randomBytes(CADU_VCDU_BYTES, seed + i)));
        return cadus;
    }

    std::vector<uint8_t> concat(const std::vector<std::vector<uint8_t>> &frames)
    {
        std::vector<uint8_t> out;
        for (auto &f : frames)
            out.insert(out.end(), f.begin(), f.end());
        return out;
    }

    // What the decoder outputs for a CADU: the ASM, then the derandomized VCDU and check symbols
    std::vector<uint8_t> decoded(std::vector<uint8_t> cadu)
    {
        derand_ccsds(&cadu[4], cadu.size() - 4);
        return cadu;
    }

    std::vector<uint8_t> decoded(const std::vector<std::vector<uint8_t>> &cadus)
    {
        std::vector<uint8_t> out;
        for (auto &c : cadus)
        {
            auto d = decoded(c);
            out.insert(out.end(), d.begin(), d.end());
        }
        return out;
    }

    // Soft symbols of these CADUs, followed by two more that only flush the decoder: the
    // Viterbi decoder outputs with a delay, so the end of the stream never comes out.
    std::vector<int8_t> softStream(const std::vector<std::vector<uint8_t>> &cadus, uint32_t seed = 999)
    {
        auto all = cadus;
        for (auto &c : makeCadus(2, seed))
            all.push_back(c);
        auto soft = softSymbols(channelSymbols(concat(all)));
        soft.resize(soft.size() + 1000, 0); // Keeps the length off a buffer boundary
        return soft;
    }

    std::shared_ptr<pl::ProcessingModule> runDecoder(const std::string &input, const std::string &hint, nlohmann::json params)
    {
        auto m = pl::ccsds::CCSDSConvConcatDecoderModule::getInstance(input, hint, params);
        m->setInputType(pl::DATA_FILE);
        m->setOutputType(pl::DATA_FILE);
        m->init();
        m->process();
        return m;
    }

    // The CADUs written to the output file (the flush CADUs of softStream() may follow)
    std::vector<uint8_t> firstCadus(const std::shared_ptr<pl::ProcessingModule> &m, size_t count)
    {
        auto out = readBytes(m->getOutput());
        if (out.size() > count * CADU_BYTES)
            out.resize(count * CADU_BYTES);
        return out;
    }
} // namespace

TEST_CASE("CCSDSConvConcatDecoderModule identifies itself", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    CHECK(pl::ccsds::CCSDSConvConcatDecoderModule::getID() == "ccsds_conv_concat_decoder");
    auto m = pl::ccsds::CCSDSConvConcatDecoderModule::getInstance("in", "out", hritParams());
    CHECK(m->getIDM() == "ccsds_conv_concat_decoder");
    CHECK(m->getInputTypes() == std::vector<pl::ModuleDataType>{pl::DATA_FILE, pl::DATA_STREAM});
    CHECK(m->getOutputTypes() == std::vector<pl::ModuleDataType>{pl::DATA_FILE, pl::DATA_STREAM});
}

TEST_CASE("CCSDSConvConcatDecoderModule rejects invalid configurations", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    auto params = hritParams();

    SECTION("unknown constellation")
    {
        params["constellation"] = "8psk";
        CHECK_THROWS_AS(pl::ccsds::CCSDSConvConcatDecoderModule::getInstance("in", "out", params), satdump::satdump_exception_t);
    }
    SECTION("unknown Reed-Solomon type")
    {
        params["rs_type"] = "rs255";
        CHECK_THROWS_AS(pl::ccsds::CCSDSConvConcatDecoderModule::getInstance("in", "out", params), satdump::satdump_exception_t);
    }
    SECTION("Reed-Solomon enabled without a type")
    {
        params.erase("rs_type");
        CHECK_THROWS_AS(pl::ccsds::CCSDSConvConcatDecoderModule::getInstance("in", "out", params), satdump::satdump_exception_t);
    }
    SECTION("missing mandatory parameters")
    {
        auto key = GENERATE("constellation", "cadu_size", "viterbi_ber_thresold", "viterbi_outsync_after", "rs_i");
        params.erase(key);
        CHECK_THROWS(pl::ccsds::CCSDSConvConcatDecoderModule::getInstance("in", "out", params));
    }
}

TEST_CASE("CCSDSConvConcatDecoderModule decodes the CADUs of a GOES-R HRIT stream", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    TempDir tmp("pipe_concat");
    auto cadus = makeCadus(10, 100);
    std::string input = tmp.file("in.soft");
    writeBytes(input, softStream(cadus));

    auto m = runDecoder(input, tmp.file("out"), hritParams());

    CHECK(m->getOutput() == tmp.file("out") + ".cadu");
    CHECK(bytesDiff(firstCadus(m, cadus.size()), decoded(cadus)) == "");

    // The lock states are those of the end of the file here, see the live stream test
    auto stats = m->getModuleStats();
    CHECK(stats["viterbi_ber"].is_number());
    CHECK(stats["deframer_lock"].is_boolean());
    CHECK(stats["rs_avg"] == 0);
    CHECK(stats["progress"].is_number());
}

TEST_CASE("CCSDSConvConcatDecoderModule is insensitive to the BPSK polarity with NRZ-M", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    TempDir tmp("pipe_concat");
    auto cadus = makeCadus(6, 200);

    // A Costas loop locks on either phase. Decoding the first bit of an NRZ-M stream needs
    // the level before it, which is taken as 0: the ASM of the first CADU is then wrong.
    auto sent = cadus;
    sent.insert(sent.begin(), makeCadu(randomBytes(CADU_VCDU_BYTES, 201)));
    auto soft = softStream(sent);
    for (auto &s : soft)
        s = -s;
    std::string input = tmp.file("in.soft");
    writeBytes(input, soft);

    auto m = runDecoder(input, tmp.file("out"), hritParams());
    CHECK(bytesDiff(firstCadus(m, cadus.size()), decoded(cadus)) == "");
}

TEST_CASE("CCSDSConvConcatDecoderModule locks on a stream starting anywhere", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    TempDir tmp("pipe_concat");
    auto cadus = makeCadus(6, 300);
    auto soft = softStream(cadus);

    // An odd number of symbols is lost first, so neither the symbol pairs nor the frames
    // line up with the buffers. The first CADU is cut, and cannot be recovered.
    soft.erase(soft.begin(), soft.begin() + 2001);
    std::string input = tmp.file("in.soft");
    writeBytes(input, soft);

    auto m = runDecoder(input, tmp.file("out"), hritParams());

    cadus.erase(cadus.begin());
    CHECK(bytesDiff(firstCadus(m, cadus.size()), decoded(cadus)) == "");
}

TEST_CASE("CCSDSConvConcatDecoderModule corrects channel errors", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    TempDir tmp("pipe_concat");
    auto cadus = makeCadus(8, 400);

    // Byte errors within the reach of the Reed-Solomon code (16 per codeword) in one CADU,
    // which the convolutional code cannot see as they are made before it
    auto sent = cadus;
    for (int i = 0; i < 40; i++)
        sent[3][100 + i * 7] ^= 0x5A;

    // And 2% of the symbols flipped, for the Viterbi decoder
    auto soft = softStream(sent);
    Lcg rng(77);
    for (auto &s : soft)
        if (rng.below(100) < 2)
            s = -s;

    std::string input = tmp.file("in.soft");
    writeBytes(input, soft);

    auto m = runDecoder(input, tmp.file("out"), hritParams());
    CHECK(bytesDiff(firstCadus(m, cadus.size()), decoded(cadus)) == "");
    CHECK(m->getModuleStats()["viterbi_ber"].get<float>() > 0.0f);
}

TEST_CASE("CCSDSConvConcatDecoderModule rs_usecheck drops the frames Reed-Solomon cannot correct", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    TempDir tmp("pipe_concat");
    auto cadus = makeCadus(6, 500);

    // Far too many errors in one CADU
    auto sent = cadus;
    for (int i = 100; i < 900; i++)
        sent[2][i] ^= 0xFF;

    std::string input = tmp.file("in.soft");
    writeBytes(input, softStream(sent));

    SECTION("enabled, as in goes_hrit")
    {
        auto m = runDecoder(input, tmp.file("out"), hritParams());

        auto good = cadus;
        good.erase(good.begin() + 2);
        CHECK(bytesDiff(firstCadus(m, good.size()), decoded(good)) == "");
    }

    SECTION("disabled: the frame is output as received")
    {
        auto params = hritParams();
        params["rs_usecheck"] = false;
        auto m = runDecoder(input, tmp.file("out"), params);

        auto out = firstCadus(m, cadus.size());
        REQUIRE(out.size() == cadus.size() * CADU_BYTES);
        auto expected = decoded(cadus);
        // Every other frame is intact
        CHECK(std::equal(out.begin(), out.begin() + 2 * CADU_BYTES, expected.begin()));
        CHECK(std::equal(out.begin() + 3 * CADU_BYTES, out.end(), expected.begin() + 3 * CADU_BYTES));
        CHECK_FALSE(std::equal(out.begin() + 2 * CADU_BYTES, out.begin() + 3 * CADU_BYTES, expected.begin() + 2 * CADU_BYTES));
    }
}

TEST_CASE("CCSDSConvConcatDecoderModule optional steps can be turned off", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    TempDir tmp("pipe_concat");
    auto cadus = makeCadus(6, 600);
    std::string input = tmp.file("in.soft");
    auto params = hritParams();

    SECTION("no derandomization: frames are output still randomized")
    {
        writeBytes(input, softStream(cadus));
        params["derandomize"] = false;
        params["rs_i"] = 0; // Reed-Solomon cannot work on randomized data
        auto m = runDecoder(input, tmp.file("out"), params);
        CHECK(bytesDiff(firstCadus(m, cadus.size()), concat(cadus)) == "");
        CHECK_FALSE(m->getModuleStats().contains("rs_avg"));
    }

    SECTION("no NRZ-M")
    {
        auto all = cadus;
        for (auto &c : makeCadus(2, 999))
            all.push_back(c);
        writeBytes(input, softSymbols(convEncode(unpackBits(concat(all)))));
        params["nrzm"] = false;
        auto m = runDecoder(input, tmp.file("out"), params);
        CHECK(bytesDiff(firstCadus(m, cadus.size()), decoded(cadus)) == "");
    }

    SECTION("not CCSDS: the output is a .frm")
    {
        writeBytes(input, softStream(cadus));
        params["ccsds"] = false;
        auto m = runDecoder(input, tmp.file("out"), params);
        CHECK(m->getOutput() == tmp.file("out") + ".frm");
        CHECK(bytesDiff(firstCadus(m, cadus.size()), decoded(cadus)) == "");
    }
}

TEST_CASE("CCSDSConvConcatDecoderModule outputs nothing for noise", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    TempDir tmp("pipe_concat");
    std::string input = tmp.file("in.soft");
    writeBytes(input, softSymbols(randomBits(8192 * 6 + 100, 42)));

    auto m = runDecoder(input, tmp.file("out"), hritParams());

    REQUIRE(std::filesystem::exists(m->getOutput()));
    CHECK(std::filesystem::file_size(m->getOutput()) == 0);
    auto stats = m->getModuleStats();
    CHECK(stats["viterbi_lock"] == 0);
    CHECK(stats["viterbi_state"] == "NOSYNC");
    CHECK(stats["deframer_lock"] == false);
}

// The Reed-Solomon results are only written for the frames the deframer outputs. The module is
// built over memory filled with 0x01, so that a read of them before the first frame would give
// 0x01010101 every time rather than whatever the heap held
TEST_CASE("CCSDSConvConcatDecoderModule reports an rs_avg of 0 until it has a frame", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    TempDir tmp("pipe_concat");
    std::string input = tmp.file("in.soft");
    writeBytes(input, softSymbols(randomBits(8192 * 6 + 100, 42)));

    auto module = makePrefilled<pl::ccsds::CCSDSConvConcatDecoderModule>(0x01, input, tmp.file("out"), hritParams());
    pl::ProcessingModule *m = module.get(); // init() is protected in the module class
    m->setInputType(pl::DATA_FILE);
    m->setOutputType(pl::DATA_FILE);
    m->init();
    CHECK(m->getModuleStats()["rs_avg"] == 0);

    m->process();
    REQUIRE(std::filesystem::file_size(m->getOutput()) == 0);
    CHECK(m->getModuleStats()["rs_avg"] == 0);
}

TEST_CASE("CCSDSConvConcatDecoderModule decodes a live stream", "[pipeline][modules][ccsds_conv_concat_decoder]")
{
    // As in a live pipeline: soft symbols come from the demodulator through a FIFO, and the
    // CADUs go to the next module through another one
    auto m = pl::ccsds::CCSDSConvConcatDecoderModule::getInstance("", "unused", hritParams());
    m->input_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
    m->output_fifo = std::make_shared<dsp::RingBuffer<uint8_t>>(1000000);
    m->setInputType(pl::DATA_STREAM);
    m->setOutputType(pl::DATA_STREAM);
    m->init();
    m->input_active = true;

    std::thread t([m]() { m->process(); });

    // The deframer only reports SYNCED after more than 10 consecutive sync markers
    auto cadus = makeCadus(14, 700);
    auto soft = softStream(cadus);
    m->input_fifo->write((uint8_t *)soft.data(), soft.size());

    size_t expected_size = cadus.size() * CADU_BYTES;
    bool got = waitUntil([&]() { return m->output_fifo->getReadable() >= (int)expected_size; });
    auto stats = m->getModuleStats();

    m->input_active = false;
    m->input_fifo->stopReader();
    m->input_fifo->stopWriter();
    t.join();

    REQUIRE(got);
    std::vector<uint8_t> out(expected_size);
    m->output_fifo->read(out.data(), expected_size);
    CHECK(bytesDiff(out, decoded(cadus)) == "");

    CHECK(stats["viterbi_lock"] == 1);
    CHECK(stats["viterbi_state"] == "SYNCED");
    CHECK(stats["viterbi_ber"].get<float>() < 0.05f);
    CHECK(stats["deframer_lock"] == true);
    CHECK(stats["deframer_state"] == "SYNCED");
    CHECK(stats["rs_avg"] == 0);
    CHECK_FALSE(stats.contains("progress")); // Only for a file
}
