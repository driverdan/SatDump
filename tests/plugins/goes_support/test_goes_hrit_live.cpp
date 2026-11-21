// "satdump legacy live goes_hrit" from end to end, with a rtl_tcp server as its source. Only
// built when the RTL-TCP plugin is built as well.
#if defined(SATDUMP_TEST_PLUGIN_RTLTCP_SUPPORT) && defined(SATDUMP_TEST_PLUGIN_XRIT_SUPPORT) && !defined(_WIN32)

#include "cli_test_helpers.h"
#include "helpers/ccsds_tx.h"
#include "helpers/random.h"
#include "helpers/wait.h"
#include "image/io.h"
#include "legacy/live.h"
#include "pipeline/pipeline_test_helpers.h"
#include "plugins_test_helpers.h"
#include "sdr_sources/rtltcp_support/rtltcp_test_helpers.h"
#include "xrit_support/xrit_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <thread>

// The test plays the part of the satellite, the dongle and rtl_tcp: LRIT files are sent the
// way GOES-R does (xrit_support/xrit_test_helpers.h), coded and BPSK modulated at 927 ksym/s
// (helpers/ccsds_tx.h), sampled at 2.4 Msps in unsigned 8-bit I/Q, and served over TCP
// (sdr_sources/rtltcp_support/rtltcp_test_helpers.h). satdump is run with the plugins loaded
// and the pipelines shipped in resources/, and must write the files that were sent.

using namespace cli_test;
using namespace pipeline_test;
using namespace xrit_test;
using Catch::Matchers::WithinAbs;
namespace pl = satdump::pipeline;

namespace
{
    const double SAMPLERATE = 2.4e6;
    const double SYMBOLRATE = 927e3;
    const int COLUMNS = 64;
    const int LINES = 24;

    // satdump as it is once started: configuration, plugins, modules and pipelines
    struct SatDumpEnv : CommandEnv
    {
        ModuleRegistryGuard reg;
        PipelinesStateGuard pipelines;
        TempDir user{"hrit_user"};
        UserPathGuard user_guard{user.str()};
        CwdGuard cwd{dir.path};
        ValueGuard<std::string> tle_override{satdump::tle_file_override};
        LogCapture log;
        std::vector<std::shared_ptr<satdump::Plugin>> plugins;
        std::string out = dir.file("live_output");

        SatDumpEnv()
        {
            auto &cfg = satdump::satdump_cfg.main_cfg;
            cfg["satdump_general"]["image_format"]["value"] = "png";
            cfg["satdump_general"]["product_format"]["value"] = "png";
            cfg["satdump_general"]["auto_process_products"]["value"] = false;
            cfg["user_interface"]["show_waterfall_demod_fft"]["value"] = false;
            cfg["user_interface"]["advanced_mode"]["value"] = false;

            for (std::string path : {SATDUMP_TEST_PLUGIN_RTLTCP_SUPPORT, SATDUMP_TEST_PLUGIN_XRIT_SUPPORT, SATDUMP_TEST_PLUGIN_GOES_SUPPORT})
                plugins.push_back(plugins_test::loadTestPlugin(path));

            pl::registerModules();
            pl::loadPipelines(std::string(SATDUMP_TEST_SOURCE_DIR) + "/resources/pipelines");
        }

        std::vector<std::string> files()
        {
            std::vector<std::string> list;
            if (std::filesystem::exists(out))
                for (auto &e : std::filesystem::recursive_directory_iterator(out))
                    if (e.is_regular_file())
                        list.push_back(std::filesystem::relative(e.path(), out).generic_string());
            std::sort(list.begin(), list.end());
            return list;
        }
    };

    Bytes linePixels(int line)
    {
        Bytes p(COLUMNS);
        for (int x = 0; x < COLUMNS; x++)
            p[x] = 20 + (line * 7 + x) % 200;
        return p;
    }

    // The downlink: fill frames while the receiver locks (the deframer reports SYNCED after
    // a dozen frames), a message, an image that loses two lines on the way, another message,
    // and fill frames again
    std::vector<Bytes> downlink(const std::string &first_text, const std::string &last_text)
    {
        std::vector<RiceLine> lines;
        for (int y = 0; y < LINES; y++)
            lines.push_back({linePixels(y), y == 10 || y == 11});

        Mux mux;
        for (int i = 0; i < 30; i++)
            mux.fill();
        mux.add(13, packetize(lritFile(2, {annotationRecord("A-FIRST.lrit"), noaaRecord(1, 0, 0, 0)}, Bytes(first_text.begin(), first_text.end())), 40));
        mux.add(13, riceImagePackets(lritHeaders(0, {imageStructureRecord(8, COLUMNS, LINES, 1), annotationRecord("IMAGE.lrit"), noaaRecord(1, 0, 0, 1)}, 0), lines, 40, 10));
        mux.add(13, packetize(lritFile(2, {annotationRecord("A-LAST.lrit"), noaaRecord(1, 0, 0, 0)}, Bytes(last_text.begin(), last_text.end())), 40, 100));
        mux.flush(13);
        for (int i = 0; i < 6; i++)
            mux.fill();
        return mux.vcdus();
    }

    // What the dongle outputs for these VCDUs: a slightly noisy BPSK signal, a bit off
    // frequency, at two thirds of full scale
    std::vector<uint8_t> receive(const std::vector<Bytes> &vcdus)
    {
        Bytes cadus;
        for (auto &v : vcdus)
        {
            Bytes c = test_helpers::makeCadu(v);
            cadus.insert(cadus.end(), c.begin(), c.end());
        }

        auto baseband = test_helpers::bpskModulate(test_helpers::channelSymbols(cadus), SAMPLERATE / SYMBOLRATE, 0.5, 0.4f, 1.0, 3e3 / SAMPLERATE);
        test_helpers::Lcg rng(4242);
        for (auto &s : baseband)
            s = s + complex_t(0.03 * rng.gauss(), 0.03 * rng.gauss());
        return test_helpers::toU8IQ(baseband);
    }

    // Polls /api while the command runs on the calling thread, keeping the statistics seen
    // while the decoder was locked, and stops the command once `done` is true (or after a
    // bounded time)
    struct Operator
    {
        nlohmann::json locked_stats;
        bool finished = false;
        std::thread th;

        Operator(int http_port, std::function<bool()> done)
        {
            th = std::thread(
                [this, http_port, done]()
                {
                    finished = test_helpers::waitUntil(
                        [&]()
                        {
                            auto response = httpRequest(http_port, "/api");
                            if (response.ok && response.status == 200)
                            {
                                auto stats = nlohmann::json::parse(response.body, nullptr, false);
                                if (stats.is_object() && stats.contains("ccsds_conv_concat_decoder") && stats["ccsds_conv_concat_decoder"]["deframer_state"] == "SYNCED")
                                    locked_stats = stats;
                            }
                            return done();
                        },
                        std::chrono::seconds(120), std::chrono::milliseconds(20));
                    live_should_exit = true; // What CTRL+C does
                });
        }

        ~Operator()
        {
            if (th.joinable())
                th.join();
        }
    };
} // namespace

TEST_CASE("goes_hrit is a live pipeline once the plugins are loaded", "[plugins][goes_support][goes_hrit]")
{
    SatDumpEnv env;

    for (auto &p : env.plugins)
        REQUIRE(p);

    pl::Pipeline pipeline = pl::getPipelineFromID("goes_hrit");
    CHECK(pipeline.name == "GOES-R HRIT");
    CHECK(pipeline.live);
    CHECK(pipeline.live_cfg.normal_live == std::vector<int>{1, 2, 3});
    CHECK(pipeline.live_cfg.server_live == std::vector<int>{1, 2});
    CHECK(pipeline.live_cfg.client_live == std::vector<int>{3});
    CHECK(pipeline.live_cfg.pkt_size == 10240);

    REQUIRE(pipeline.steps.size() == 4);
    CHECK(pipeline.steps[0].level == "baseband");
    CHECK(pipeline.steps[1].module == "psk_demod");
    CHECK(pipeline.steps[2].module == "ccsds_conv_concat_decoder");
    CHECK(pipeline.steps[3].module == "goes_lrit_data_decoder");
    CHECK(pipeline.steps[1].parameters["symbolrate"] == SYMBOLRATE);

    // --fill_missing reaches the decoder, over the default of the pipeline
    CHECK(pipeline.editable_parameters["fill_missing"]["value"] == false);
    nlohmann::json flags = {{"fill_missing", true}, {"samplerate", 2400000}};
    auto params = pl::Pipeline::prepareParameters(pipeline.steps[3].parameters, flags);
    CHECK(params["fill_missing"] == true);
    CHECK(params["max_fill_lines"] == 50);
    CHECK(params["write_images"] == true);

    // Every module of the pipeline exists
    for (int step : pipeline.live_cfg.normal_live)
        CHECK(pl::moduleExists(pipeline.steps[step].module));
}

TEST_CASE("satdump legacy live goes_hrit receives HRIT files from a rtl_tcp server", "[plugins][goes_support][goes_hrit]")
{
    SatDumpEnv env;
    std::string first_text = "GOES-R HRIT ADMINISTRATIVE MESSAGE\r\nFirst file of the test.\r\n";
    std::string last_text = "Last file of the test.\r\n";

    // Sent in real time, about a second of signal
    rtltcp_test::FakeRtlTcp server(receive(downlink(first_text, last_text)), true, SAMPLERATE);
    int http_port = test_helpers::closedPort();
    Operator op(http_port, [&]() { return std::filesystem::exists(env.out + "/Admin Messages/A-LAST.txt"); });

    // The command line of a station with a rtl_tcp server on its network. Only the addresses
    // differ: both are on this machine.
    int ret =
        call(main_live, {"satdump legacy", "live",  "goes_hrit",   env.out,    "--source", "rtltcp", "--ip_address",  "127.0.0.1",         "--port",        std::to_string(server.port()), "--lna_agc",
                         "--samplerate",   "2.4e6", "--frequency", "1694.1e6", "--gain",   "49",     "--http_server", loopback(http_port), "--fill_missing"});
    op.th.join();

    INFO(env.log.dump());
    CHECK(ret == 0);
    REQUIRE(op.finished);

    // The files that were sent are written
    CHECK(env.files() == std::vector<std::string>{"Admin Messages/A-FIRST.txt", "Admin Messages/A-LAST.txt", "IMAGES/IMAGE.lrit.png"});
    CHECK(readFile(env.out + "/Admin Messages/A-FIRST.txt") == first_text);
    CHECK(readFile(env.out + "/Admin Messages/A-LAST.txt") == last_text);

    // --fill_missing: the two lines lost on the way are a copy of the next one
    image::Image img;
    image::load_img(img, env.out + "/IMAGES/IMAGE.lrit.png");
    REQUIRE(img.width() == (size_t)COLUMNS);
    REQUIRE(img.height() == (size_t)LINES);
    int wrong_lines = 0;
    for (int y = 0; y < LINES; y++)
    {
        Bytes expected = linePixels(y == 10 || y == 11 ? 12 : y), row(COLUMNS);
        for (int x = 0; x < COLUMNS; x++)
            row[x] = img.get(0, x, y);
        wrong_lines += row != expected;
    }
    CHECK(wrong_lines == 0);

    // The dongle is set up from the command line: --samplerate, --frequency, --lna_agc
    auto commands = server.commands();
    REQUIRE(commands.size() >= 3);
    CHECK(commands[0] == rtltcp_test::Command{rtltcp_test::CMD_SAMPLERATE, 2400000});
    CHECK(commands[1] == rtltcp_test::Command{rtltcp_test::CMD_FREQUENCY, 1694100000});
    CHECK(commands[2] == rtltcp_test::Command{rtltcp_test::CMD_AGC_MODE, 1});

    // Stopping turns the bias tee off and closes the connection
    CHECK(commands.back() == rtltcp_test::Command{rtltcp_test::CMD_BIAS_TEE, 0});
    CHECK(server.waitForDisconnect());

    // --http_server: the webserver reported the state of every module while locked
    CHECK(env.log.contains("Start webserver on " + loopback(http_port), slog::LOG_INFO));
    auto &stats = op.locked_stats;
    REQUIRE(stats.is_object());
    REQUIRE(stats.contains("psk_demod"));
    REQUIRE(stats.contains("goes_lrit_data_decoder"));
    CHECK(stats["psk_demod"]["snr"].get<float>() > 5.0f);
    CHECK(stats["psk_demod"]["peak_snr"].get<float>() > 10.0f);
    CHECK_THAT(stats["psk_demod"]["freq"].get<float>(), WithinAbs(3e3, 2e3));
    CHECK(stats["ccsds_conv_concat_decoder"]["viterbi_state"] == "SYNCED");
    CHECK(stats["ccsds_conv_concat_decoder"]["viterbi_ber"].get<float>() < 0.1f);
    CHECK(stats["ccsds_conv_concat_decoder"]["rs_avg"].get<int>() >= 0);
    CHECK_FALSE(portIsOpen(http_port)); // And is stopped with the command

    // The command stopped cleanly on the signal
    CHECK(env.log.contains("Signal Received. Stopping.", slog::LOG_WARN));
    CHECK(env.log.contains("Demodulation finished", slog::LOG_INFO));
    CHECK_FALSE(env.log.contains("Fatal error"));
    CHECK_FALSE(env.log.contains("Timeout is over!"));
}

TEST_CASE("satdump legacy live goes_hrit fails without a rtl_tcp server", "[plugins][goes_support][goes_hrit]")
{
    SatDumpEnv env;
    live_should_exit = true;

    int ret = call(main_live, {"satdump legacy", "live", "goes_hrit", env.out, "--source", "rtltcp", "--ip_address", "127.0.0.1", "--port", std::to_string(test_helpers::closedPort()), "--samplerate",
                               "2.4e6", "--frequency", "1694.1e6"});

    INFO(env.log.dump());
    CHECK(ret == 1);
    CHECK(env.log.contains("Fatal error running pipeline/device : Could not connect to RTL-TCP", slog::LOG_ERROR));
}
#endif
