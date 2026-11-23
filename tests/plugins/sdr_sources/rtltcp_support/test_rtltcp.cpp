#include "cli_test_helpers.h"
#include "core/exception.h"
#include "helpers/compare.h"
#include "helpers/guards.h"
#include "helpers/satdump_guards.h"
#include "helpers/sockets.h"
#include "plugins_test_helpers.h"
#include "sdr_sources/rtltcp_support/rtltcp.h"
#include "sdr_sources/rtltcp_support/rtltcp_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <future>
#include <memory>
#include <netdb.h>

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

    // Sets a source up like "satdump legacy live" does it, pointed at a server on port
    std::shared_ptr<dsp::DSPSampleSource> setUp(std::shared_ptr<dsp::DSPSampleSource> source, int port, nlohmann::json settings = nlohmann::json::object(), uint64_t frequency = 1694100000,
                                                uint64_t samplerate = 2400000)
    {
        if (!settings.contains("ip_address"))
            settings["ip_address"] = "127.0.0.1";
        settings["port"] = port;

        source->open();
        source->set_frequency(frequency);
        source->set_samplerate(samplerate);
        source->set_settings(settings);
        return source;
    }

    std::shared_ptr<dsp::DSPSampleSource> makeSource(FakeRtlTcp &server, nlohmann::json settings = nlohmann::json::object(), uint64_t frequency = 1694100000, uint64_t samplerate = 2400000)
    {
        return setUp(RTLTCPSource::getInstance(RTLTCPSource::getAvailableSources()[0]), server.port(), settings, frequency, samplerate);
    }

    // A RTLTCPSource that tries to reconnect with other delays than the default ones, which
    // add up to about 20 seconds
    class QuickRetrySource : public RTLTCPSource
    {
    public:
        QuickRetrySource(int attempts, int first_delay_ms, int max_delay_ms, int connect_timeout_ms = 2000) : RTLTCPSource(RTLTCPSource::getAvailableSources()[0])
        {
            reconnect_attempts = attempts;
            reconnect_first_delay_ms = first_delay_ms;
            reconnect_max_delay_ms = max_delay_ms;
            reconnect_connect_timeout_ms = connect_timeout_ms;
        }
    };

    // A port on which connecting hangs, as with a host that does not answer: the listening
    // socket never accepts and its queue is full, so the kernel drops the SYNs. ready() says
    // whether the socket could be set up, hangs() whether a connection attempt did hang (if
    // not, the blackhole is closed again).
    class Blackhole
    {
    private:
        int listen_fd = -1, filler = -1;
        int listen_port;
        bool is_ready = false;
        bool does_hang = false;

        static sockaddr_in loopback(int port)
        {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            return addr;
        }

        void closeAll()
        {
            if (filler >= 0)
                close(filler);
            if (listen_fd >= 0)
                close(listen_fd);
            filler = listen_fd = -1;
        }

    public:
        explicit Blackhole(int port) : listen_port(port)
        {
            sockaddr_in addr = loopback(port);
            listen_fd = socket(AF_INET, SOCK_STREAM, 0);
            int reuse = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) < 0 || listen(listen_fd, 0) < 0)
            {
                closeAll();
                return;
            }
            is_ready = true;

            // Fills the queue
            filler = socket(AF_INET, SOCK_STREAM, 0);
            connect(filler, (sockaddr *)&addr, sizeof(addr));

            // Then a connection must not complete
            int probe = socket(AF_INET, SOCK_STREAM, 0);
            fcntl(probe, F_SETFL, fcntl(probe, F_GETFL, 0) | O_NONBLOCK);
            if (connect(probe, (sockaddr *)&addr, sizeof(addr)) < 0 && errno == EINPROGRESS)
            {
                pollfd p{probe, POLLOUT, 0};
                does_hang = poll(&p, 1, 200) == 0;
            }
            close(probe);
            if (!does_hang)
                closeAll();
        }

        ~Blackhole() { closeAll(); }

        bool ready() const { return is_ready; }
        bool hangs() const { return does_hang; }
        int port() const { return listen_port; }
    };

    // Whether host resolves to at least one address of family (AF_INET or AF_INET6)
    bool resolvesTo(const char *host, int family)
    {
        addrinfo hints{};
        hints.ai_family = family;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *result = nullptr;
        if (getaddrinfo(host, "1234", &hints, &result) != 0)
            return false;
        freeaddrinfo(result);
        return true;
    }

    // A list of 127.0.0.1 addresses with these ports, as getaddrinfo() gives them, to try
    // several addresses one after the other without depending on the resolver
    struct AddressList
    {
        std::vector<sockaddr_in> addrs;
        std::vector<addrinfo> infos;

        explicit AddressList(const std::vector<int> &ports) : addrs(ports.size()), infos(ports.size())
        {
            for (size_t i = 0; i < ports.size(); i++)
            {
                addrs[i] = {};
                addrs[i].sin_family = AF_INET;
                addrs[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addrs[i].sin_port = htons(ports[i]);
                infos[i] = {};
                infos[i].ai_family = AF_INET;
                infos[i].ai_socktype = SOCK_STREAM;
                infos[i].ai_protocol = IPPROTO_TCP;
                infos[i].ai_addr = (sockaddr *)&addrs[i];
                infos[i].ai_addrlen = sizeof(sockaddr_in);
                infos[i].ai_next = i + 1 < ports.size() ? &infos[i + 1] : nullptr;
            }
        }

        const addrinfo *get() const { return infos.data(); }
    };

    double secondsSince(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }

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

    // A Drain that counts the samples it reads
    struct Counter
    {
        std::shared_ptr<dsp::DSPSampleSource> source;
        std::atomic<size_t> samples{0};
        std::thread th;

        explicit Counter(std::shared_ptr<dsp::DSPSampleSource> s) : source(s)
        {
            auto stream = source->output_stream;
            th = std::thread(
                [this, stream]()
                {
                    int n;
                    while ((n = stream->read()) > 0)
                    {
                        samples += n;
                        stream->flush();
                    }
                });
        }

        ~Counter()
        {
            source->output_stream->stopReader();
            th.join();
        }
    };

    // Waits until the count has not changed for a while, and returns it
    size_t settledCount(Counter &counter)
    {
        size_t last = counter.samples;
        for (int i = 0; i < 50; i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            size_t now = counter.samples;
            if (now == last)
                return now;
            last = now;
        }
        return last;
    }
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

    SECTION("lna_agc and tuner_agc given with the legacy agc setting win over it")
    {
        source->set_settings({{"agc", true}, {"lna_agc", false}});
        auto s = source->get_settings();
        CHECK(s["lna_agc"] == false);
        CHECK(s["tuner_agc"] == true);

        source->set_settings({{"agc", false}, {"tuner_agc", true}});
        s = source->get_settings();
        CHECK(s["lna_agc"] == false);
        CHECK(s["tuner_agc"] == true);
        CHECK_FALSE(s.contains("agc"));
    }
}

// The two AGCs are independent settings, as for the USB RTL-SDR source: enabling the LNA AGC
// alone must not enable the tuner AGC too, which would stop the manual gain from being sent
TEST_CASE("RTLTCPSource lna_agc leaves the tuner AGC alone", "[plugins][sdr_sources][rtltcp]")
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

TEST_CASE("RTLTCPSource start fails when the host cannot be resolved", "[plugins][sdr_sources][rtltcp]")
{
    auto source = RTLTCPSource::getInstance(RTLTCPSource::getAvailableSources()[0]);
    source->open();
    source->set_frequency(100000000);
    source->set_samplerate(2400000);
    source->set_settings({{"ip_address", "nonexistent.invalid"}, {"port", 1234}});

    CHECK_THROWS_AS(source->start(), satdump::satdump_exception_t);
    CHECK_NOTHROW(source->stop());

    RTLTCPClient client;
    CHECK_FALSE(client.connectToRTL((char *)"nonexistent.invalid", 1234));
    CHECK(client.connectError().find("could not resolve nonexistent.invalid") == 0);
}

// The POSIX client used to resolve with gethostbyname() into an AF_INET socket only
TEST_CASE("RTLTCPSource connects to a server on the IPv6 loopback address", "[plugins][sdr_sources][rtltcp]")
{
    if (!FakeRtlTcp::ipv6Available())
        SKIP("no IPv6 loopback here");

    FakeRtlTcp server({}, true, 0, AfterIQ::Silence, 0, "::1");
    auto source = setUp(RTLTCPSource::getInstance(RTLTCPSource::getAvailableSources()[0]), server.port(), {{"ip_address", "::1"}, {"tuner_agc", true}, {"lna_agc", false}});
    source->start();
    {
        Drain drain(source);
        CHECK(server.waitForCommands(6) == Commands{{CMD_SAMPLERATE, 2400000}, {CMD_FREQUENCY, 1694100000}, {CMD_AGC_MODE, 0}, {CMD_GAIN_MODE, 0}, {CMD_BIAS_TEE, 0}, {CMD_PPM, 0}});
        source->stop();
    }
    CHECK(server.connected());
}

// "localhost" usually resolves to both ::1 and 127.0.0.1, while a rtl_tcp server may listen on
// one of them only: every address is tried, not only the first one
TEST_CASE("RTLTCPSource tries every address of a host name", "[plugins][sdr_sources][rtltcp]")
{
    std::string address = GENERATE("127.0.0.1", "::1");
    int family = address == "::1" ? AF_INET6 : AF_INET;
    if (family == AF_INET6 && !FakeRtlTcp::ipv6Available())
        SKIP("no IPv6 loopback here");
    if (!resolvesTo("localhost", family))
        SKIP("localhost does not resolve to " + address + " here");

    CAPTURE(address);
    FakeRtlTcp server({}, true, 0, AfterIQ::Silence, 0, address);
    RTLTCPClient client;
    REQUIRE(client.connectToRTL((char *)"localhost", server.port()));
    CHECK(client.setFrequency(137100000));
    CHECK(server.waitForCommands(1) == Commands{{CMD_FREQUENCY, 137100000}});

    // The same with the non-blocking attempts used to reconnect
    client.disconnect();
    FakeRtlTcp server2({}, true, 0, AfterIQ::Silence, 0, address);
    REQUIRE(client.connectToRTL((char *)"localhost", server2.port(), []() { return true; }, 2000));
    CHECK(client.setFrequency(137100000));
    CHECK(server2.waitForCommands(1) == Commands{{CMD_FREQUENCY, 137100000}});
    client.disconnect();
}

// With a timeout, each address gets a share of the time left: one that does not answer (an
// AAAA record behind a firewall that drops packets) used to take all of it, so the next ones
// were never tried when reconnecting
TEST_CASE("RTLTCPClient tries the next addresses when one does not answer", "[plugins][sdr_sources][rtltcp]")
{
    Blackhole hole1(test_helpers::closedPort());
    Blackhole hole2(test_helpers::closedPort());
    if (!hole1.hangs() || !hole2.hangs())
        SKIP("connecting to a full listening queue does not hang here");

    SECTION("two addresses that do not answer, then one that does")
    {
        FakeRtlTcp server;
        AddressList list({hole1.port(), hole2.port(), server.port()});
        RTLTCPClient client;
        auto t = std::chrono::steady_clock::now();
        REQUIRE(client.connectToAddresses(list.get(), []() { return true; }, 1500));
        double elapsed = secondsSince(t);
        CHECK(elapsed < 1.5);
        CHECK(elapsed > 0.6); // The two first attempts did time out
        CHECK(client.connectError().empty());
        CHECK(client.setFrequency(137100000));
        CHECK(server.waitForCommands(1) == Commands{{CMD_FREQUENCY, 137100000}});
        client.disconnect();
    }

    SECTION("the timeout covers all the attempts")
    {
        AddressList list({hole1.port(), hole2.port(), hole1.port()});
        RTLTCPClient client;
        auto t = std::chrono::steady_clock::now();
        CHECK_FALSE(client.connectToAddresses(list.get(), []() { return true; }, 600));
        CHECK(secondsSince(t) < 1.0);
        CHECK(client.connectError().find("timed out") != std::string::npos);
    }
}

TEST_CASE("RTLTCPClient connectError is cleared once an address connects", "[plugins][sdr_sources][rtltcp]")
{
    FakeRtlTcp server;
    AddressList list({test_helpers::closedPort(), server.port()});
    RTLTCPClient client;

    SECTION("blocking") { REQUIRE(client.connectToAddresses(list.get())); }
    SECTION("with a timeout")
    {
        REQUIRE(client.connectToAddresses(list.get(), []() { return true; }, 2000));
    }

    CHECK(client.connectError().empty());
    client.disconnect();
}

TEST_CASE("RTLTCPClient connectToAddresses fails on an empty list", "[plugins][sdr_sources][rtltcp]")
{
    RTLTCPClient client;
    CHECK_FALSE(client.connectToAddresses(nullptr));
    CHECK(client.connectError() == "no address to connect to");
    CHECK_FALSE(client.connectToAddresses(nullptr, []() { return true; }, 1000));
    CHECK(client.connectError() == "no address to connect to");
    CHECK_FALSE(client.setFrequency(137100000));
}

// An address of a family the system has no socket for (a socket() error), or one that
// connect() refuses at once (not EINPROGRESS, even without blocking), is skipped
TEST_CASE("RTLTCPClient skips an address it cannot even try", "[plugins][sdr_sources][rtltcp]")
{
    bool with_timeout = GENERATE(false, true);
    bool bad_family = GENERATE(true, false);
    CAPTURE(with_timeout, bad_family);

    auto connect = [&](RTLTCPClient &client, const addrinfo *list)
    {
        if (with_timeout)
            return client.connectToAddresses(list, []() { return true; }, 2000);
        return client.connectToAddresses(list);
    };

    FakeRtlTcp server;
    AddressList list({test_helpers::closedPort(), server.port()});
    if (bad_family)
        list.infos[0].ai_family = AF_UNSPEC;
    else
        list.infos[0].ai_addrlen = 1; // EINVAL

    SECTION("the next address is tried")
    {
        RTLTCPClient client;
        REQUIRE(connect(client, list.get()));
        CHECK(client.connectError().empty());
        CHECK(client.setFrequency(137100000));
        CHECK(server.waitForCommands(1) == Commands{{CMD_FREQUENCY, 137100000}});
        client.disconnect();
    }

    SECTION("alone, why it failed is kept")
    {
        list.infos[0].ai_next = nullptr;
        RTLTCPClient client;
        auto t = std::chrono::steady_clock::now();
        CHECK_FALSE(connect(client, list.get()));
        CHECK(secondsSince(t) < 0.5); // No wait
        CHECK(client.connectError() == strerror(bad_family ? EAFNOSUPPORT : EINVAL));
        CHECK_FALSE(client.setFrequency(137100000));
    }
}

// An attempt only ends between two polls, so it may take more than its share of the timeout:
// the next addresses are then not tried at all, and the error says both why
TEST_CASE("RTLTCPClient does not try more addresses once the timeout is spent", "[plugins][sdr_sources][rtltcp]")
{
    Blackhole hole(test_helpers::closedPort());
    if (!hole.hangs())
        SKIP("connecting to a full listening queue does not hang here");

    FakeRtlTcp server;
    AddressList list({hole.port(), server.port()});
    RTLTCPClient client;
    CHECK_FALSE(client.connectToAddresses(list.get(), []() { return true; }, 1));
    CHECK(client.connectError() == "timed out, then timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(server.connected());
}

TEST_CASE("RTLTCPClient cancelling a connection does not try the next addresses", "[plugins][sdr_sources][rtltcp]")
{
    Blackhole hole(test_helpers::closedPort());
    if (!hole.hangs())
        SKIP("connecting to a full listening queue does not hang here");

    FakeRtlTcp server;
    AddressList list({hole.port(), server.port()});
    RTLTCPClient client;
    auto t = std::chrono::steady_clock::now();
    std::atomic<int> polls{0};
    CHECK_FALSE(client.connectToAddresses(list.get(), [&]() { return ++polls < 4; }, 0));
    CHECK(secondsSince(t) < 1.0);
    CHECK(client.connectError() == "cancelled");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(server.connected());
}

TEST_CASE("RTLTCPClient keeps its connection when asked to connect again", "[plugins][sdr_sources][rtltcp]")
{
    FakeRtlTcp first, second;
    RTLTCPClient client;
    REQUIRE(client.connectToRTL((char *)"127.0.0.1", first.port()));

    CHECK(client.connectToRTL((char *)"127.0.0.1", second.port()));
    AddressList list({second.port()});
    CHECK(client.connectToAddresses(list.get()));

    CHECK(client.setFrequency(137100000));
    CHECK(first.waitForCommands(1) == Commands{{CMD_FREQUENCY, 137100000}});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(second.connected());
    client.disconnect();
}

TEST_CASE("RTLTCPClient drops the commands sent without a connection", "[plugins][sdr_sources][rtltcp]")
{
    RTLTCPClient client;
    CHECK_FALSE(client.setFrequency(137100000));
    CHECK_NOTHROW(client.disconnect()); // Never connected

    FakeRtlTcp server;
    REQUIRE(client.connectToRTL((char *)"127.0.0.1", server.port()));
    CHECK(client.setFrequency(137100000));
    REQUIRE(server.waitForCommands(1).size() == 1);

    client.disconnect();
    CHECK(server.waitForDisconnect());
    CHECK_FALSE(client.setFrequency(137500000));
    CHECK_FALSE(client.setBiasTee(false));
    CHECK_NOTHROW(client.disconnect()); // Twice
    CHECK(server.commands().size() == 1);
}

// The 5 bytes of a command: its number, then its parameter as a big-endian 32-bit integer
TEST_CASE("RTLTCPClient encodes the commands as rtl_tcp expects them", "[plugins][sdr_sources][rtltcp]")
{
    CHECK(sizeof(command_t) == 5);

    FakeRtlTcp server;
    RTLTCPClient client;
    REQUIRE(client.connectToRTL((char *)"127.0.0.1", server.port()));

    CHECK(client.setFrequency(1694100000));
    CHECK(client.setFrequency(4294967295.0)); // The largest one
    CHECK(client.setSampleRate(2400000));
    CHECK(client.setGainMode(1));
    CHECK(client.setGain(49.6 * 10)); // Truncated
    CHECK(client.setPPM(-3));         // Two's complement
    CHECK(client.setAGCMode(1));
    CHECK(client.setDirectSampling(2)); // Q branch
    CHECK(client.setOffsetTuning(true));
    CHECK(client.setGainIndex(28));
    CHECK(client.setBiasTee(true));
    CHECK(client.sendCommand(0xAB, 0x01020304));

    CHECK(server.waitForCommands(12) == Commands{{CMD_FREQUENCY, 1694100000},
                                                 {CMD_FREQUENCY, 4294967295u},
                                                 {CMD_SAMPLERATE, 2400000},
                                                 {CMD_GAIN_MODE, 1},
                                                 {CMD_GAIN, 496},
                                                 {CMD_PPM, 0xFFFFFFFDu},
                                                 {CMD_AGC_MODE, 1},
                                                 {CMD_DIRECT_SAMPLING, 2},
                                                 {CMD_OFFSET_TUNING, 1},
                                                 {CMD_GAIN_INDEX, 28},
                                                 {CMD_BIAS_TEE, 1},
                                                 {0xAB, 0x01020304}});
    client.disconnect();
}

// The source always waits with keep_going, the blocking read is the default of the client
TEST_CASE("RTLTCPClient receiveData fills the whole buffer", "[plugins][sdr_sources][rtltcp]")
{
    bool with_keep_going = GENERATE(false, true);
    CAPTURE(with_keep_going);

    std::vector<uint8_t> iq(20000);
    for (size_t i = 0; i < iq.size(); i++)
        iq[i] = (i * 7) & 0xFF;
    // Paced, so that the data comes in many pieces (about 100 ms in all)
    FakeRtlTcp server(iq, true, 100000, AfterIQ::Close);
    RTLTCPClient client;
    REQUIRE(client.connectToRTL((char *)"127.0.0.1", server.port()));

    auto receive = [&](uint8_t *buf, size_t count)
    {
        if (with_keep_going)
            return client.receiveData(buf, count, []() { return true; });
        return client.receiveData(buf, count);
    };

    std::vector<uint8_t> expected = dongleInfo();
    expected.insert(expected.end(), iq.begin(), iq.end());
    std::vector<uint8_t> got(expected.size());
    REQUIRE(receive(got.data(), got.size()));
    CHECK(test_helpers::bytesDiff(got, expected) == "");

    // Then the server leaves
    uint8_t more[16];
    CHECK_FALSE(receive(more, sizeof(more)));
    CHECK(client.receiveError() == "the server closed the connection");
    client.disconnect();
}

TEST_CASE("RTLTCPClient receiveData reads nothing once cancelled", "[plugins][sdr_sources][rtltcp]")
{
    FakeRtlTcp server;
    RTLTCPClient client;
    REQUIRE(client.connectToRTL((char *)"127.0.0.1", server.port()));
    REQUIRE(test_helpers::waitUntil([&]() { return server.connected(); }, std::chrono::seconds(10)));

    uint8_t buf[12];
    CHECK_FALSE(client.receiveData(buf, sizeof(buf), []() { return false; }));
    CHECK(client.receiveError() == "cancelled");

    // What the server sent is still there
    REQUIRE(client.receiveData(buf, sizeof(buf)));
    CHECK(std::vector<uint8_t>(buf, buf + sizeof(buf)) == dongleInfo());
    client.disconnect();
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

// The source part of "satdump legacy live ... --source rtltcp --lna_agc --gain 49": the tuner
// AGC stays off, so the manual gain is sent (see "RTLTCPSource lna_agc leaves the tuner AGC alone")
TEST_CASE("RTLTCPSource sends the gain when only the LNA AGC is enabled", "[plugins][sdr_sources][rtltcp]")
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

    SECTION("switching to the tuner AGC: the gain is not sent")
    {
        source->set_settings({{"tuner_agc", true}, {"lna_agc", true}, {"gain", 33}});
        auto commands = server.waitForCommands(at_start + 4);
        REQUIRE(commands.size() == at_start + 4);
        Commands changes(commands.begin() + at_start, commands.end());
        CHECK(changes == Commands{{CMD_AGC_MODE, 1}, {CMD_GAIN_MODE, 0}, {CMD_BIAS_TEE, 0}, {CMD_PPM, 0}});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(server.commands().size() == at_start + 4);
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

// stop() used to wait for the worker thread while it was blocked reading samples that never
// came, as when the network drops without the connection being closed: satdump could not be
// stopped, CTRL+C hung
TEST_CASE("RTLTCPSource stop does not hang when the server stops sending", "[plugins][sdr_sources][rtltcp]")
{
    std::vector<uint8_t> iq(8192 * 2, 100); // Two whole buffers of the source
    auto server = std::make_unique<FakeRtlTcp>(iq, false, 0, AfterIQ::Stall);
    auto source = makeSource(*server, {{"bias", true}});
    source->start();
    {
        Counter counter(source);
        REQUIRE(server->waitForCommands(7).size() == 7);
        // Every sample was output: the worker thread is now waiting for more
        REQUIRE(test_helpers::waitUntil([&]() { return counter.samples == iq.size() / 2; }, std::chrono::seconds(10)));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto t = std::chrono::steady_clock::now();
        auto done = std::async(std::launch::async, [&]() { source->stop(); });
        bool stopped = done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        if (!stopped)
            server.reset(); // Closes the connection, which ends the read and lets stop() return
        done.wait();
        REQUIRE(stopped);
        CHECK(secondsSince(t) < 1.0);
    }

    // The bias tee is still turned off, and the connection closed
    CHECK(server->waitForDisconnect());
    auto commands = server->commands();
    REQUIRE(commands.size() == 8);
    CHECK(commands.back() == Command{CMD_BIAS_TEE, 0});
}

// When rtl_tcp exits, the source used to output its last buffer again and again, forever,
// with a core busy: the read error was ignored. It now logs that the connection was lost and
// outputs nothing more until it has reconnected (here the server does not come back, and
// stop() ends the reconnection attempts).
TEST_CASE("RTLTCPSource stops outputting samples when the server closes the connection", "[plugins][sdr_sources][rtltcp]")
{
    // Three whole buffers of the source, without the dongle information
    std::vector<uint8_t> iq(8192 * 3);
    for (size_t i = 0; i < iq.size(); i++)
        iq[i] = i & 0xFF;

    cli_test::LogCapture log;
    FakeRtlTcp server(iq, false, 0, AfterIQ::Close);
    auto source = makeSource(server);
    source->start();
    {
        Counter counter(source);
        REQUIRE(server.waitForDisconnect());

        // Exactly what the server sent, and then nothing more
        size_t count = settledCount(counter);
        CHECK(count == iq.size() / 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(counter.samples == count);

        INFO(log.dump());
        CHECK(log.contains("RTL-TCP connection lost (the server closed the connection), trying to reconnect", slog::LOG_ERROR));

        CHECK_NOTHROW(source->stop());
    }
}

TEST_CASE("RTLTCPSource stops outputting samples on a read error", "[plugins][sdr_sources][rtltcp]")
{
    cli_test::LogCapture log;
    FakeRtlTcp server(std::vector<uint8_t>(8192 * 3, 100), false, 0, AfterIQ::Reset);
    auto source = makeSource(server);
    source->start();
    {
        Counter counter(source);
        REQUIRE(server.waitForDisconnect());

        // Some of what was sent may be lost with the reset, but nothing is made up
        size_t count = settledCount(counter);
        CHECK(count <= 8192 * 3 / 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(counter.samples == count);

        INFO(log.dump());
        CHECK(log.contains("RTL-TCP connection lost (receive error: ", slog::LOG_ERROR));

        CHECK_NOTHROW(source->stop());
    }
}

// The commands were written with write(), and writing to a connection the server closed
// raised SIGPIPE, which ended the program: changing a setting or stopping the source after
// rtl_tcp exited was enough
TEST_CASE("RTLTCPSource commands sent after the server left do not end the program", "[plugins][sdr_sources][rtltcp]")
{
    FakeRtlTcp server(std::vector<uint8_t>(8192, 100), false, 0, AfterIQ::Close);
    auto source = makeSource(server, {{"tuner_agc", false}, {"bias", true}});
    source->start();
    {
        Drain drain(source);
        REQUIRE(server.waitForDisconnect());

        // The first write after the close gets a reset from the server, the next ones fail
        for (int i = 0; i < 5; i++)
        {
            source->set_frequency(1694100000 + i);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        source->set_settings({{"gain", 20}, {"bias", false}});
        CHECK_NOTHROW(source->stop());
    }
    CHECK(source->get_frequency() == 1694100004);
}

// Once stopped, the source connects again on the next start
TEST_CASE("RTLTCPSource connects again when started after the connection was lost", "[plugins][sdr_sources][rtltcp]")
{
    FakeRtlTcp first(std::vector<uint8_t>(8192, 100), false, 0, AfterIQ::Close);
    auto source = makeSource(first);
    source->start();
    {
        Drain drain(source);
        REQUIRE(first.waitForDisconnect());
        source->stop();
    }

    std::vector<uint8_t> iq(8192 * 2, 200);
    FakeRtlTcp second(iq, false);
    source->set_settings({{"port", second.port()}});
    source->start();
    auto samples = readSamples(source, 8192);
    source->output_stream->stopReader();
    source->stop();

    CHECK(second.connected());
    REQUIRE(samples.size() >= 8192);
    CHECK_THAT(samples[8191].imag, WithinAbs((200 - 127.4f) / 128.0f, 1e-6));
}

TEST_CASE("RTLTCPSource reconnects and configures the dongle again when the server comes back", "[plugins][sdr_sources][rtltcp]")
{
    cli_test::LogCapture log;
    std::vector<uint8_t> first_iq(8192 * 3, 100);
    auto first = std::make_unique<FakeRtlTcp>(first_iq, false, 0, AfterIQ::Close);
    int port = first->port();

    auto source = setUp(std::make_shared<QuickRetrySource>(50, 20, 50), port, {{"lna_agc", true}, {"tuner_agc", false}, {"gain", 49}});
    source->start();
    REQUIRE(first->waitForCommands(7).size() == 7);
    {
        Counter counter(source);
        REQUIRE(first->waitForDisconnect());

        // Nothing is output while the server is gone
        size_t before = settledCount(counter);
        CHECK(before == first_iq.size() / 2);
        first.reset();

        // A change made meanwhile is sent on reconnection
        source->set_frequency(1694500000);

        FakeRtlTcp second(std::vector<uint8_t>(8192 * 2, 200), false, 0, AfterIQ::Silence, port);
        CHECK(second.waitForCommands(7) == Commands{{CMD_SAMPLERATE, 2400000}, {CMD_FREQUENCY, 1694500000}, {CMD_AGC_MODE, 1}, {CMD_GAIN_MODE, 1}, {CMD_GAIN, 490}, {CMD_BIAS_TEE, 0}, {CMD_PPM, 0}});

        // And the samples flow again
        CHECK(test_helpers::waitUntil([&]() { return counter.samples >= before + 8192; }, std::chrono::seconds(10)));

        INFO(log.dump());
        CHECK(log.contains("RTL-TCP connection lost (the server closed the connection), trying to reconnect", slog::LOG_ERROR));
        CHECK(log.contains("RTL-TCP reconnected", slog::LOG_INFO));
        CHECK_FALSE(log.contains("could not reconnect"));
        CHECK_FALSE(source->has_ended());

        source->stop();
        CHECK(second.waitForDisconnect());
    }
}

TEST_CASE("RTLTCPSource gives up reconnecting after a few attempts when the server stays gone", "[plugins][sdr_sources][rtltcp]")
{
    cli_test::LogCapture log;
    FakeRtlTcp server(std::vector<uint8_t>(8192, 100), false, 0, AfterIQ::Close);
    auto source = setUp(std::make_shared<QuickRetrySource>(3, 10, 20), server.port());
    source->start();
    {
        Counter counter(source);
        REQUIRE(server.waitForDisconnect());
        REQUIRE(test_helpers::waitUntil([&]() { return log.contains("RTL-TCP could not reconnect after 3 attempts", slog::LOG_ERROR); }, std::chrono::seconds(10)));

        INFO(log.dump());
        CHECK(log.contains("RTL-TCP reconnection attempt 3/3 failed (Connection refused)", slog::LOG_WARN));
        CHECK_FALSE(log.contains("attempt 4/"));

        // The source says it ended, for "satdump legacy live" to stop instead of idling
        CHECK(test_helpers::waitUntil([&]() { return source->has_ended(); }, std::chrono::seconds(10)));

        // Then idle: no samples, and next to no CPU
        size_t count = counter.samples;
        std::clock_t cpu_start = std::clock();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        double cpu = double(std::clock() - cpu_start) / CLOCKS_PER_SEC;
        CHECK(counter.samples == count);
        CHECK(cpu < 0.2);

        auto t = std::chrono::steady_clock::now();
        source->stop();
        CHECK(secondsSince(t) < 1.0);
        CHECK(source->has_ended());
    }

    // Until it is started again
    FakeRtlTcp second(std::vector<uint8_t>(8192 * 2, 200), false);
    source->set_settings({{"port", second.port()}});
    source->start();
    CHECK_FALSE(source->has_ended());
    auto samples = readSamples(source, 8192);
    CHECK(samples.size() >= 8192);
    CHECK_FALSE(source->has_ended());
    source->output_stream->stopReader();
    source->stop();
}

TEST_CASE("RTLTCPSource stop interrupts the wait before a reconnection attempt", "[plugins][sdr_sources][rtltcp]")
{
    cli_test::LogCapture log;
    FakeRtlTcp server(std::vector<uint8_t>(8192, 100), false, 0, AfterIQ::Close);
    auto source = setUp(std::make_shared<QuickRetrySource>(5, 60000, 60000), server.port());
    source->start();
    {
        Drain drain(source);
        REQUIRE(server.waitForDisconnect());
        REQUIRE(test_helpers::waitUntil([&]() { return log.contains("trying to reconnect"); }, std::chrono::seconds(10)));

        auto t = std::chrono::steady_clock::now();
        source->stop();
        CHECK(secondsSince(t) < 1.0);

        INFO(log.dump());
        CHECK_FALSE(log.contains("reconnection attempt"));
        // Stopped, it did not end by itself
        CHECK_FALSE(source->has_ended());
    }
}

// Without a deadline, an attempt on a host that does not answer lasted as long as the system
// tries to connect (about 2 minutes on Linux)
TEST_CASE("RTLTCPSource gives a reconnection attempt up when the host does not answer", "[plugins][sdr_sources][rtltcp]")
{
    cli_test::LogCapture log;
    FakeRtlTcp server(std::vector<uint8_t>(8192, 100), false, 0, AfterIQ::Close);
    int port = server.port();
    // The first attempt is late enough for the blackhole to be up
    auto source = setUp(std::make_shared<QuickRetrySource>(2, 1000, 1000, 300), port);
    source->start();
    {
        Drain drain(source);
        REQUIRE(server.waitForDisconnect());
        auto t = std::chrono::steady_clock::now();
        Blackhole hole(port);
        REQUIRE(hole.ready());
        if (!hole.hangs())
            SKIP("connecting to a full listening queue does not hang here");

        REQUIRE(test_helpers::waitUntil([&]() { return log.contains("RTL-TCP could not reconnect after 2 attempts", slog::LOG_ERROR); }, std::chrono::seconds(10)));
        CHECK(secondsSince(t) < 4.0);

        INFO(log.dump());
        CHECK(log.contains("RTL-TCP reconnection attempt 1/2 failed (timed out)", slog::LOG_WARN));
        CHECK(log.contains("RTL-TCP reconnection attempt 2/2 failed (timed out)", slog::LOG_WARN));

        source->stop();
    }
}

TEST_CASE("RTLTCPSource stop interrupts a reconnection attempt", "[plugins][sdr_sources][rtltcp]")
{
    cli_test::LogCapture log;
    FakeRtlTcp server(std::vector<uint8_t>(8192, 100), false, 0, AfterIQ::Close);
    int port = server.port();
    auto source = setUp(std::make_shared<QuickRetrySource>(1, 1000, 1000, 60000), port);
    source->start();
    {
        Drain drain(source);
        REQUIRE(server.waitForDisconnect());
        Blackhole hole(port);
        REQUIRE(hole.ready());
        if (!hole.hangs())
            SKIP("connecting to a full listening queue does not hang here");

        // Well into the attempt, which starts 1 second after the connection was lost
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        auto t = std::chrono::steady_clock::now();
        source->stop();
        CHECK(secondsSince(t) < 1.0);

        INFO(log.dump());
        CHECK_FALSE(log.contains("reconnection attempt 1/1 failed"));
        CHECK_FALSE(log.contains("could not reconnect"));
    }
}

// The status page of autotrack (on the HTTP thread) and the recorder read the samplerate and
// the settings while the worker thread configures the dongle again after each reconnection
TEST_CASE("RTLTCPSource settings can be read while it reconnects", "[plugins][sdr_sources][rtltcp]")
{
    auto server = std::make_unique<FakeRtlTcp>(std::vector<uint8_t>(8192, 100), false, 0, AfterIQ::Close);
    int port = server->port();
    auto source = setUp(std::make_shared<QuickRetrySource>(200, 5, 10), port);
    source->start();
    {
        Drain drain(source);

        std::atomic<bool> reading{true};
        std::atomic<size_t> reads{0};
        std::thread reader(
            [&]()
            {
                while (reading)
                {
                    if (source->get_samplerate() == 2400000 && source->get_settings()["port"] == port)
                        reads++;
                    std::this_thread::yield();
                }
            });

        // Each server sends a little and leaves, then the next one comes on the same port
        const int RECONNECTIONS = 3;
        for (int i = 0; i < RECONNECTIONS; i++)
        {
            REQUIRE(server->waitForDisconnect());
            server.reset();
            server = std::make_unique<FakeRtlTcp>(std::vector<uint8_t>(8192, 100), false, 0, AfterIQ::Close, port);
            CHECK(server->waitForCommands(7).size() == 7); // Configured again
        }

        reading = false;
        reader.join();
        CHECK(reads > 0);
        source->stop();
    }
}
#endif
