#include "core/exception.h"
#include "helpers/guards.h"
#include "helpers/satdump_guards.h"
#include "helpers/sockets.h"
#include "plugins_test_helpers.h"
#include "sdr_sources/rtltcp_support/rtltcp.h"
#include "sdr_sources/rtltcp_support/rtltcp_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// The RTL-TCP source is run against a rtl_tcp server made by the test, which records the
// commands it is sent and plays back samples. No dongle and no rtl_tcp are involved.

#ifndef _WIN32
using namespace rtltcp_test;
using Catch::Matchers::WithinAbs;

namespace
{
    using test_helpers::EventBusGuard;
    using test_helpers::ValueGuard;
    using Commands = std::vector<Command>;

    // A source set up like "satdump legacy live" does it, pointed at the server
    std::shared_ptr<dsp::DSPSampleSource> makeSource(FakeRtlTcp &server, nlohmann::json settings = nlohmann::json::object(), uint64_t frequency = 1694100000, uint64_t samplerate = 2400000)
    {
        settings["ip_address"] = "127.0.0.1";
        settings["port"] = server.port();

        auto source = RTLTCPSource::getInstance(RTLTCPSource::getAvailableSources()[0]);
        source->open();
        source->set_frequency(frequency);
        source->set_samplerate(samplerate);
        source->set_settings(settings);
        return source;
    }

    // Reads `count` samples from the source (bounded by the reads of the stream itself:
    // the server never stops sending)
    std::vector<complex_t> readSamples(std::shared_ptr<dsp::DSPSampleSource> source, size_t count)
    {
        std::vector<complex_t> out;
        while (out.size() < count)
        {
            int n = source->output_stream->read();
            if (n <= 0)
                break;
            out.insert(out.end(), source->output_stream->readBuf, source->output_stream->readBuf + n);
            source->output_stream->flush();
        }
        return out;
    }

    // Keeps the stream of a started source moving, which a pipeline otherwise does: the
    // source blocks as soon as nothing reads its samples
    struct Drain
    {
        std::shared_ptr<dsp::DSPSampleSource> source;
        std::thread th;

        explicit Drain(std::shared_ptr<dsp::DSPSampleSource> s) : source(s)
        {
            auto stream = source->output_stream;
            th = std::thread(
                [stream]()
                {
                    while (stream->read() > 0)
                        stream->flush();
                });
        }

        ~Drain()
        {
            source->output_stream->stopReader();
            th.join();
        }
    };
} // namespace

TEST_CASE("RTLTCPSource describes itself as a single network device", "[plugins][sdr_sources][rtltcp]")
{
    CHECK(RTLTCPSource::getID() == "rtltcp");

    auto sources = RTLTCPSource::getAvailableSources();
    REQUIRE(sources.size() == 1);
    CHECK(sources[0].source_type == "rtltcp");
    CHECK(sources[0].name == "RTL-TCP");
    CHECK(sources[0].unique_id == "0");
}

TEST_CASE("rtltcp_support registers the source when the plugin is loaded", "[plugins][sdr_sources][rtltcp]")
{
    EventBusGuard bus;
    ValueGuard<std::map<std::string, dsp::RegisteredSource>> registry(dsp::dsp_sources_registry);
    dsp::dsp_sources_registry.clear();

    auto plugin = plugins_test::loadTestPlugin(SATDUMP_TEST_PLUGIN_RTLTCP_SUPPORT);
    REQUIRE(plugin);
    CHECK(plugin->getID() == "rtltcp_support");

    // What "satdump legacy live --source rtltcp" does to find it
    dsp::registerAllSources();
    REQUIRE(dsp::dsp_sources_registry.count("rtltcp") == 1);

    dsp::SourceDescriptor found;
    for (auto &src : dsp::getAllAvailableSources())
        if (src.source_type == "rtltcp")
            found = src;
    CHECK(found.name == "RTL-TCP");

    auto source = dsp::getSourceFromDescriptor(found);
    REQUIRE(source);
    CHECK(source->d_sdr_id == "0");
    CHECK(std::dynamic_pointer_cast<RTLTCPSource>(source) != nullptr);
}

TEST_CASE("RTLTCPSource settings have defaults and are read back", "[plugins][sdr_sources][rtltcp]")
{
    auto source = RTLTCPSource::getInstance(RTLTCPSource::getAvailableSources()[0]);

    SECTION("defaults")
    {
        auto s = source->get_settings();
        CHECK(s["ip_address"] == "0.0.0.0");
        CHECK(s["port"] == 1234);
        CHECK(s["gain"] == 10);
        CHECK(s["lna_agc"] == false);
        CHECK(s["tuner_agc"] == false);
        CHECK(s["bias"] == false);
        CHECK(s["ppm_correction"] == 0);
    }

    SECTION("every setting")
    {
        source->set_settings({{"ip_address", "192.168.8.225"}, {"port", 4321}, {"gain", 49}, {"lna_agc", true}, {"tuner_agc", false}, {"bias", true}, {"ppm_correction", -3}});
        auto s = source->get_settings();
        CHECK(s["ip_address"] == "192.168.8.225");
        CHECK(s["port"] == 4321);
        CHECK(s["gain"] == 49);
        CHECK(s["lna_agc"] == true);
        CHECK(s["tuner_agc"] == false);
        CHECK(s["bias"] == true);
        CHECK(s["ppm_correction"] == -3);
    }

    SECTION("settings that are absent or of the wrong type keep their value")
    {
        source->set_settings({{"port", 4321}, {"gain", 30}});
        source->set_settings({{"port", "not a port"}, {"gain", nullptr}, {"frequency", 1694.1e6}});
        auto s = source->get_settings();
        CHECK(s["port"] == 4321);
        CHECK(s["gain"] == 30);
        CHECK(s["frequency"] == 1694.1e6); // Other flags of the command line are kept as they are
    }

    SECTION("the legacy agc setting sets both AGCs, and is removed")
    {
        source->set_settings({{"agc", true}});
        auto s = source->get_settings();
        CHECK(s["lna_agc"] == true);
        CHECK(s["tuner_agc"] == true);
        CHECK_FALSE(s.contains("agc"));
    }
}

// Bug: plugins/sdr_sources/rtltcp_support/rtltcp.cpp:59 - tuner_agc defaults to the value of
// lna_agc instead of keeping its own (rtlsdr_sdr.cpp:123 has the intended line). Enabling
// the LNA AGC alone therefore enables the tuner AGC as well, and a manual gain is then never
// sent: "--lna_agc --gain 49" runs at whatever gain the tuner AGC picks.
TEST_CASE("RTLTCPSource lna_agc leaves the tuner AGC alone", "[plugins][sdr_sources][rtltcp][!shouldfail]")
{
    auto source = RTLTCPSource::getInstance(RTLTCPSource::getAvailableSources()[0]);

    SECTION("enabling the LNA AGC")
    {
        source->set_settings({{"lna_agc", true}, {"gain", 49}});
        CHECK(source->get_settings()["tuner_agc"] == false);
    }

    SECTION("disabling the LNA AGC")
    {
        source->set_settings({{"tuner_agc", true}});
        source->set_settings({{"lna_agc", false}});
        CHECK(source->get_settings()["tuner_agc"] == true);
    }
}

TEST_CASE("RTLTCPSource accepts the samplerates of a RTL-SDR", "[plugins][sdr_sources][rtltcp]")
{
    auto source = RTLTCPSource::getInstance(RTLTCPSource::getAvailableSources()[0]);
    source->open();

    SECTION("listed samplerates")
    {
        uint64_t rate = GENERATE(250000, 1024000, 2048000, 2400000, 3200000);
        CHECK_NOTHROW(source->set_samplerate(rate));
        CHECK(source->get_samplerate() == rate);
    }

    SECTION("any other one up to 3.2 Msps")
    {
        CHECK_NOTHROW(source->set_samplerate(1800000));
        CHECK(source->get_samplerate() == 1800000);
    }
}

// Bug: src-core/common/widgets/double_list.cpp:77 - DoubleList::set_value() ignores manual_max
// (the check is commented out), so the 3.2 Msps limit RTLTCPSource::set_samplerate() passes
// is not applied and "Unsupported samplerate" is never thrown. A samplerate no RTL-SDR can do
// (such as the 6 Msps preset of most pipelines) is sent to the dongle as is.
TEST_CASE("RTLTCPSource refuses samplerates above 3.2 Msps", "[plugins][sdr_sources][rtltcp][!shouldfail]")
{
    auto source = RTLTCPSource::getInstance(RTLTCPSource::getAvailableSources()[0]);
    source->open();
    source->set_samplerate(2400000);

    CHECK_THROWS_AS(source->set_samplerate(6000000), satdump::satdump_exception_t);
    CHECK(source->get_samplerate() == 2400000);
}

TEST_CASE("RTLTCPSource start fails when there is no server", "[plugins][sdr_sources][rtltcp]")
{
    auto source = RTLTCPSource::getInstance(RTLTCPSource::getAvailableSources()[0]);
    source->open();
    source->set_frequency(100000000);
    source->set_samplerate(2400000);
    source->set_settings({{"ip_address", "127.0.0.1"}, {"port", test_helpers::closedPort()}});

    CHECK_THROWS_AS(source->start(), satdump::satdump_exception_t);
    CHECK_NOTHROW(source->stop());
}

TEST_CASE("RTLTCPSource start configures the dongle", "[plugins][sdr_sources][rtltcp]")
{
    FakeRtlTcp server;

    SECTION("manual gain")
    {
        auto source = makeSource(server, {{"lna_agc", true}, {"tuner_agc", false}, {"gain", 49}});
        source->start();
        Drain drain(source);

        // Samplerate and frequency, then the AGC of the RTL2832, manual gain mode and the
        // gain in tenths of dB, the bias tee and the frequency correction
        CHECK(server.waitForCommands(7) == Commands{{CMD_SAMPLERATE, 2400000}, {CMD_FREQUENCY, 1694100000}, {CMD_AGC_MODE, 1}, {CMD_GAIN_MODE, 1}, {CMD_GAIN, 490}, {CMD_BIAS_TEE, 0}, {CMD_PPM, 0}});
        source->stop();
    }

    SECTION("tuner AGC: no gain is sent")
    {
        auto source = makeSource(server, {{"tuner_agc", true}, {"lna_agc", false}, {"gain", 49}});
        source->start();
        Drain drain(source);

        CHECK(server.waitForCommands(6) == Commands{{CMD_SAMPLERATE, 2400000}, {CMD_FREQUENCY, 1694100000}, {CMD_AGC_MODE, 0}, {CMD_GAIN_MODE, 0}, {CMD_BIAS_TEE, 0}, {CMD_PPM, 0}});
        source->stop();
    }

    SECTION("bias tee, frequency correction and another samplerate")
    {
        auto source = makeSource(server, {{"tuner_agc", false}, {"gain", 20}, {"bias", true}, {"ppm_correction", 12}}, 137100000, 1024000);
        source->start();
        Drain drain(source);

        CHECK(server.waitForCommands(7) == Commands{{CMD_SAMPLERATE, 1024000}, {CMD_FREQUENCY, 137100000}, {CMD_AGC_MODE, 0}, {CMD_GAIN_MODE, 1}, {CMD_GAIN, 200}, {CMD_BIAS_TEE, 1}, {CMD_PPM, 12}});
        source->stop();
    }

    CHECK(server.connected());
}

// Bug: see "RTLTCPSource lna_agc leaves the tuner AGC alone" above. This is the source part
// of "satdump legacy live ... --source rtltcp --lna_agc --gain 49".
TEST_CASE("RTLTCPSource sends the gain when only the LNA AGC is enabled", "[plugins][sdr_sources][rtltcp][!shouldfail]")
{
    FakeRtlTcp server;
    auto source = makeSource(server, {{"lna_agc", true}, {"gain", 49}});
    source->start();
    Drain drain(source);

    auto commands = server.waitForCommands(7, std::chrono::seconds(2));
    source->stop();

    CHECK(commands == Commands{{CMD_SAMPLERATE, 2400000}, {CMD_FREQUENCY, 1694100000}, {CMD_AGC_MODE, 1}, {CMD_GAIN_MODE, 1}, {CMD_GAIN, 490}, {CMD_BIAS_TEE, 0}, {CMD_PPM, 0}});
}

TEST_CASE("RTLTCPSource applies changes while it is started", "[plugins][sdr_sources][rtltcp]")
{
    FakeRtlTcp server;
    auto source = makeSource(server, {{"tuner_agc", false}, {"gain", 10}});

    // Nothing is sent before start, there is no connection yet
    CHECK_FALSE(server.connected());

    source->start();
    Drain drain(source);
    size_t at_start = server.waitForCommands(7).size();
    REQUIRE(at_start == 7);

    SECTION("frequency")
    {
        source->set_frequency(1694500000);
        auto commands = server.waitForCommands(at_start + 1);
        REQUIRE(commands.size() == at_start + 1);
        CHECK(commands.back() == Command{CMD_FREQUENCY, 1694500000});
        CHECK(source->get_frequency() == 1694500000);
    }

    SECTION("settings: gains, bias tee and frequency correction are all sent again")
    {
        source->set_settings({{"tuner_agc", false}, {"gain", 33}, {"bias", true}, {"ppm_correction", -2}});
        auto commands = server.waitForCommands(at_start + 5);
        REQUIRE(commands.size() == at_start + 5);
        Commands changes(commands.begin() + at_start, commands.end());
        CHECK(changes == Commands{{CMD_AGC_MODE, 0}, {CMD_GAIN_MODE, 1}, {CMD_GAIN, 330}, {CMD_BIAS_TEE, 1}, {CMD_PPM, (uint32_t)-2}});
    }

    source->stop();
}

TEST_CASE("RTLTCPSource converts the unsigned 8-bit samples of the dongle", "[plugins][sdr_sources][rtltcp]")
{
    // Every value of a byte on I, and the opposite ramp on Q, several times over
    std::vector<uint8_t> iq;
    for (int i = 0; i < 4096 * 4; i++)
    {
        iq.push_back(i & 0xFF);
        iq.push_back(255 - (i & 0xFF));
    }

    // Without the dongle information rtl_tcp starts with, see the next test
    FakeRtlTcp server(iq, false);
    auto source = makeSource(server);
    source->start();

    auto samples = readSamples(source, iq.size() / 2);
    source->output_stream->stopReader();
    source->stop();

    REQUIRE(samples.size() >= iq.size() / 2);
    float worst = 0;
    for (size_t i = 0; i < iq.size() / 2; i++)
    {
        worst = std::max(worst, std::abs(samples[i].real - (iq[i * 2] - 127.4f) / 128.0f));
        worst = std::max(worst, std::abs(samples[i].imag - (iq[i * 2 + 1] - 127.4f) / 128.0f));
    }
    CHECK_THAT(worst, WithinAbs(0.0, 1e-6));

    // The whole range of the dongle maps to just under +-1
    CHECK_THAT(samples[0].real, WithinAbs(-0.9953, 1e-4));
    CHECK_THAT(samples[255].real, WithinAbs(0.9969, 1e-4));
}

TEST_CASE("RTLTCPSource outputs the dongle information of rtl_tcp as samples", "[plugins][sdr_sources][rtltcp]")
{
    // rtl_tcp greets its client with 12 bytes ("RTL0", tuner type, number of gains), which
    // the source does not skip: they come out as 6 samples before those of the dongle
    std::vector<uint8_t> iq(8192 * 2, 200);
    FakeRtlTcp server(iq);
    auto source = makeSource(server);
    source->start();

    auto samples = readSamples(source, 4096);
    source->output_stream->stopReader();
    source->stop();

    REQUIRE(samples.size() >= 4096);
    auto info = dongleInfo();
    CHECK_THAT(samples[0].real, WithinAbs((info[0] - 127.4f) / 128.0f, 1e-6)); // 'R'
    CHECK_THAT(samples[0].imag, WithinAbs((info[1] - 127.4f) / 128.0f, 1e-6)); // 'T'
    CHECK_THAT(samples[6].real, WithinAbs((200 - 127.4f) / 128.0f, 1e-6));
    CHECK_THAT(samples[4095].imag, WithinAbs((200 - 127.4f) / 128.0f, 1e-6));
}

TEST_CASE("RTLTCPSource stop turns the bias tee off and disconnects", "[plugins][sdr_sources][rtltcp]")
{
    FakeRtlTcp server;
    auto source = makeSource(server, {{"tuner_agc", false}, {"bias", true}});
    source->start();
    {
        Drain drain(source);
        REQUIRE(server.waitForCommands(7).size() == 7);
        source->stop();
    }

    CHECK(server.waitForDisconnect());
    auto commands = server.commands();
    REQUIRE(commands.size() == 8);
    CHECK(commands.back() == Command{CMD_BIAS_TEE, 0});

    // Stopping again, and destroying the source, do nothing more
    CHECK_NOTHROW(source->stop());
    source.reset();
    CHECK(server.commands().size() == 8);
}
#endif
