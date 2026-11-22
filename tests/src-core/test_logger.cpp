#include "common/utils.h" // timegm on MSVC
#include "helpers/guards.h"
#include "helpers/temp_files.h"
#include "logger.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

// These tests use their own slog::Logger instances and never touch the global logger or its
// sinks. Note that Logger::add_sink replays the process-wide init buffer (messages logged before
// completeLoggerInit(), which the test main never calls), so recording sinks are cleared after
// being added.

// The init buffer is made of data symbols of satdump_core that no header declares, so they are not
// exported from the Windows DLL: the test that uses them is skipped with MSVC
#ifndef _MSC_VER
// Defined (non-static) in logger.cpp
extern std::vector<slog::LogMsg> init_log_buffer;
extern bool init_buffer_active;
#endif

namespace
{
    class RecordingSink : public slog::LoggerSink
    {
    public:
        std::mutex mtx;
        std::vector<slog::LogMsg> msgs;

        void receive(slog::LogMsg log)
        {
            if (log.lvl >= sink_lvl)
            {
                std::lock_guard<std::mutex> l(mtx);
                msgs.push_back(log);
            }
        }

        void clear()
        {
            std::lock_guard<std::mutex> l(mtx);
            msgs.clear();
        }

        // Exposes the protected formatter
        std::string format(slog::LogMsg m, bool color, int *cpos = nullptr) { return format_log(m, color, cpos); }
    };

    struct LoggerWithSink
    {
        slog::Logger logger;
        std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
        LoggerWithSink()
        {
            logger.add_sink(sink);
            sink->clear();
        }
    };

    using test_helpers::readFile;

    const std::string TIMESTAMP_RE = "\\[\\d\\d:\\d\\d:\\d\\d - \\d\\d/\\d\\d/\\d{4}\\]";
} // namespace

TEST_CASE("Logger level defaults to trace and can be changed", "[logger]")
{
    slog::Logger l;
    CHECK(l.get_level() == slog::LOG_TRACE);
    l.set_level(slog::LOG_ERROR);
    CHECK(l.get_level() == slog::LOG_ERROR);
}

TEST_CASE("Logger delivers each level with the right tag", "[logger]")
{
    LoggerWithSink t;
    t.logger.trace("t");
    t.logger.debug("d");
    t.logger.info("i");
    t.logger.notice("n");
    t.logger.warn("w");
    t.logger.error("e");
    t.logger.critical("c");

    REQUIRE(t.sink->msgs.size() == 7);
    const slog::LogLevel expected[] = {slog::LOG_TRACE, slog::LOG_DEBUG, slog::LOG_INFO, slog::LOG_NOTICE, slog::LOG_WARN, slog::LOG_ERROR, slog::LOG_CRIT};
    for (int i = 0; i < 7; i++)
        CHECK(t.sink->msgs[i].lvl == expected[i]);
    CHECK(t.sink->msgs[0].str == "t");
    CHECK(t.sink->msgs[6].str == "c");
}

TEST_CASE("Logger drops messages below its level", "[logger]")
{
    LoggerWithSink t;
    t.logger.set_level(slog::LOG_WARN);
    t.logger.debug("hidden");
    t.logger.info("hidden");
    t.logger.warn("shown warn");
    t.logger.critical("shown crit");

    REQUIRE(t.sink->msgs.size() == 2);
    CHECK(t.sink->msgs[0].lvl == slog::LOG_WARN);
    CHECK(t.sink->msgs[1].lvl == slog::LOG_CRIT);

    t.sink->clear();
    t.logger.set_level(slog::LOG_OFF);
    t.logger.critical("nothing gets through");
    CHECK(t.sink->msgs.empty());
}

namespace
{
    void logf_at(slog::Logger &l, slog::LogLevel lvl, std::string fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        l.logf(lvl, fmt, args);
        va_end(args);
    }
} // namespace

// LOG_OFF is only a threshold. A message logged at it would index one past the end of every
// sink's level tables (colors, log_schar, colors_win, log_lvls_a), so it must not reach a sink
TEST_CASE("Logger drops messages logged at LOG_OFF", "[logger]")
{
    LoggerWithSink t;
    t.logger.log(slog::LOG_OFF, std::string("off string"));
    t.logger.log(slog::LOG_OFF, 5);
    logf_at(t.logger, slog::LOG_OFF, "off %d", 1);
    CHECK(t.sink->msgs.empty());

    // Also when the logger and the sink are set to LOG_OFF, where `lvl >= level` holds
    t.logger.set_level(slog::LOG_OFF);
    t.sink->set_level(slog::LOG_OFF);
    t.logger.log(slog::LOG_OFF, std::string("off string"));
    CHECK(t.sink->msgs.empty());

    // Valid levels still get through afterwards
    t.logger.set_level(slog::LOG_TRACE);
    t.sink->set_level(slog::LOG_TRACE);
    t.logger.critical("c");
    REQUIRE(t.sink->msgs.size() == 1);
    CHECK(t.sink->msgs[0].lvl == slog::LOG_CRIT);
}

// The init buffer is replayed into every sink added later, so a LOG_OFF message must not be
// buffered either. Buffering stops for good after 1000 messages from any logger of the process,
// so the test turns it back on, with an empty buffer, to always check the replay. This needs
// data symbols of satdump_core that are not exported from the Windows DLL
#ifndef _MSC_VER
TEST_CASE("Logger does not replay LOG_OFF messages to sinks added later", "[logger]")
{
    test_helpers::ValueGuard<std::vector<slog::LogMsg>> buffer_guard(init_log_buffer, std::vector<slog::LogMsg>());
    test_helpers::ValueGuard<bool> active_guard(init_buffer_active, true);

    slog::Logger l;
    l.log(slog::LOG_OFF, std::string("logged at off, must not be replayed"));
    l.info("replayed");

    auto late = std::make_shared<RecordingSink>();
    l.add_sink(late);
    REQUIRE(late->msgs.size() == 1);
    CHECK(late->msgs[0].str == "replayed");
    CHECK(late->msgs[0].lvl == slog::LOG_INFO);
}

// The init buffer is shared by every Logger, while each Logger has its own sink_mtx. Without a
// lock of its own, two loggers on two threads raced on it and lost messages, and so did
// completeLoggerInit() against a thread that logs (seen with ThreadSanitizer)
TEST_CASE("Logger init buffer can be used from several threads", "[logger]")
{
    test_helpers::ValueGuard<std::vector<slog::LogMsg>> buffer_guard(init_log_buffer, std::vector<slog::LogMsg>());
    test_helpers::ValueGuard<bool> active_guard(init_buffer_active, true);

    SECTION("two loggers keep every message")
    {
        slog::Logger a, b;
        std::thread ta(
            [&a]
            {
                for (int i = 0; i < 400; i++)
                    a.info("a");
            });
        std::thread tb(
            [&b]
            {
                for (int i = 0; i < 400; i++)
                    b.info("b");
            });
        ta.join();
        tb.join();

        auto late = std::make_shared<RecordingSink>();
        a.add_sink(late);
        CHECK(late->msgs.size() == 800);
    }

    SECTION("completeLoggerInit while another thread logs")
    {
        slog::Logger l;
        std::thread t(
            [&l]
            {
                for (int i = 0; i < 500; i++)
                    l.info("m");
            });
        completeLoggerInit();
        t.join();

        CHECK(init_log_buffer.empty());
        CHECK_FALSE(init_buffer_active);
    }
}
#endif

TEST_CASE("Logger printf-style formatting", "[logger]")
{
    LoggerWithSink t;
    t.logger.info("%d-%s-%.2f-%c-%%", 42, "abc", 3.14159, 'z');
    t.logger.warn("no arguments");
    REQUIRE(t.sink->msgs.size() == 2);
    CHECK(t.sink->msgs[0].str == "42-abc-3.14-z-%");
    CHECK(t.sink->msgs[1].str == "no arguments");
}

TEST_CASE("Logger formats messages longer than its 1024 byte buffer", "[logger]")
{
    LoggerWithSink t;
    std::string big(3000, 'q');
    t.logger.error("<%s>", big.c_str());
    REQUIRE(t.sink->msgs.size() == 1);
    const std::string &got = t.sink->msgs[0].str;
    CHECK(got.size() == big.size() + 2);
    CHECK((got == "<" + big + ">"));
}

// Messages around the 1024 byte buffer size, which needs room for the terminating NUL
TEST_CASE("Logger keeps every character of a 1024 character message", "[logger]")
{
    LoggerWithSink t;
    for (size_t len : {1022, 1023, 1024, 1025})
    {
        std::string exact(len, 'x');
        t.logger.info("%s", exact.c_str());
    }
    REQUIRE(t.sink->msgs.size() == 4);
    CHECK(t.sink->msgs[0].str == std::string(1022, 'x'));
    CHECK(t.sink->msgs[1].str == std::string(1023, 'x'));
    CHECK(t.sink->msgs[2].str == std::string(1024, 'x'));
    CHECK(t.sink->msgs[3].str == std::string(1025, 'x'));
}

// LogMsg::str must hold exactly the formatted text, as some sinks use it directly rather than
// through c_str()
TEST_CASE("Logger formatted messages have no trailing NUL padding", "[logger]")
{
    LoggerWithSink t;
    t.logger.info("value=%d", 5);
    REQUIRE(t.sink->msgs.size() == 1);
    CHECK(t.sink->msgs[0].str == "value=5");
    CHECK(t.sink->msgs[0].str.size() == 7);
}

TEST_CASE("Logger int overloads log the decimal value", "[logger]")
{
    LoggerWithSink t;
    t.logger.trace(-12);
    t.logger.info(0);
    t.logger.warn(123456);
    t.logger.error(7);
    t.logger.critical(8);
    REQUIRE(t.sink->msgs.size() == 5);
    CHECK(t.sink->msgs[0].str == "-12");
    CHECK(t.sink->msgs[0].lvl == slog::LOG_TRACE);
    CHECK(t.sink->msgs[1].str == "0");
    CHECK(t.sink->msgs[2].str == "123456");
    CHECK(t.sink->msgs[2].lvl == slog::LOG_WARN);
    CHECK(t.sink->msgs[3].lvl == slog::LOG_ERROR);
    CHECK(t.sink->msgs[4].lvl == slog::LOG_CRIT);
}

TEST_CASE("Logger notice int overload logs at notice level", "[logger]")
{
    LoggerWithSink t;
    t.logger.notice(3);
    REQUIRE(t.sink->msgs.size() == 1);
    CHECK(t.sink->msgs[0].lvl == slog::LOG_NOTICE);
}

TEST_CASE("Logger log with a string is passed through unformatted", "[logger]")
{
    LoggerWithSink t;
    t.logger.log(slog::LOG_NOTICE, std::string("100% literal %s"));
    REQUIRE(t.sink->msgs.size() == 1);
    CHECK(t.sink->msgs[0].str == "100% literal %s");
    CHECK(t.sink->msgs[0].lvl == slog::LOG_NOTICE);
}

TEST_CASE("Logger fans out to every sink and del_sink removes one", "[logger]")
{
    slog::Logger l;
    auto a = std::make_shared<RecordingSink>();
    auto b = std::make_shared<RecordingSink>();
    l.add_sink(a);
    l.add_sink(b);
    a->clear();
    b->clear();

    l.info("both");
    CHECK(a->msgs.size() == 1);
    CHECK(b->msgs.size() == 1);

    l.del_sink(a);
    l.info("only b");
    CHECK(a->msgs.size() == 1);
    CHECK(b->msgs.size() == 2);

    // Removing a sink that is not attached is a no-op
    auto stranger = std::make_shared<RecordingSink>();
    CHECK_NOTHROW(l.del_sink(stranger));
    CHECK_NOTHROW(l.del_sink(a));
    l.info("still b");
    CHECK(b->msgs.size() == 3);
}

TEST_CASE("LoggerSink format_log plain layout", "[logger]")
{
    RecordingSink s;
    std::string out = s.format({"hello world", slog::LOG_INFO}, false);
    CHECK(std::regex_match(out, std::regex(TIMESTAMP_RE + " \\(I\\) hello world\n")));

    const char tags[] = {'T', 'D', 'I', 'N', 'W', 'E', 'C'};
    for (int lvl = slog::LOG_TRACE; lvl <= slog::LOG_CRIT; lvl++)
    {
        std::string o = s.format({"m", (slog::LogLevel)lvl}, false);
        CHECK(o.substr(o.size() - 6) == std::string("(") + tags[lvl] + ") m\n");
    }
}

TEST_CASE("LoggerSink format_log timestamp is the current UTC time", "[logger]")
{
    RecordingSink s;
    time_t before = time(nullptr);
    std::string out = s.format({"x", slog::LOG_INFO}, false);
    time_t after = time(nullptr);

    std::tm tm{};
    REQUIRE(sscanf(out.c_str(), "[%d:%d:%d - %d/%d/%d]", &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &tm.tm_mday, &tm.tm_mon, &tm.tm_year) == 6);
    tm.tm_mon -= 1;
    tm.tm_year -= 1900;
    time_t parsed = timegm(&tm);
    CHECK(parsed >= before);
    CHECK(parsed <= after);
}

TEST_CASE("LoggerSink format_log colored layout and color position", "[logger]")
{
    RecordingSink s;
    int cpos = -1;
    std::string out = s.format({"careful", slog::LOG_WARN}, true, &cpos);
    CHECK(std::regex_match(out, std::regex(TIMESTAMP_RE + " \033\\[33m\033\\[1m\\(W\\) careful\033\\[m\n")));

    // cpos marks where the level tag starts in the uncolored layout
    std::string plain = s.format({"careful", slog::LOG_WARN}, false, &cpos);
    CHECK(cpos == 24);
    CHECK(plain.substr(cpos) == "(W) careful\n");
}

// Formatted lines at and beyond vformat's 1024 byte buffer, which needs room for the NUL
TEST_CASE("LoggerSink format_log keeps the newline of long lines", "[logger]")
{
    RecordingSink s;
    // 24 byte timestamp + "(I) " + message + '\n'
    for (size_t len : {994, 995, 996, 1500})
    {
        std::string big(len, 'b');
        std::string out = s.format({big, slog::LOG_INFO}, false);
        CHECK(out.size() == 24 + 4 + big.size() + 1);
        CHECK(out.back() == '\n');
        CHECK(out.substr(28, len) == big);
    }
}

TEST_CASE("FileSink writes formatted lines and respects its level", "[logger]")
{
    test_helpers::TempDir tmp("logger");
    auto base = tmp.path / "log";
    std::filesystem::path file = base.string() + ".log";
    {
        auto sink = std::make_shared<slog::FileSink>(base.string());
        REQUIRE(std::filesystem::exists(file));
        sink->receive({"first line", slog::LOG_INFO});
        sink->set_level(slog::LOG_ERROR);
        sink->receive({"filtered", slog::LOG_WARN});
        sink->receive({"second line", slog::LOG_ERROR});
    }

    std::string content = readFile(file);
    CHECK(std::regex_match(content, std::regex(TIMESTAMP_RE + " \\(I\\) first line\n" + TIMESTAMP_RE + " \\(E\\) second line\n")));
}

TEST_CASE("FileSink never overwrites an existing log", "[logger]")
{
    test_helpers::TempDir tmp("logger");
    auto base = (tmp.path / "rotate").string();
    {
        slog::FileSink a(base);
        slog::FileSink b(base);
        slog::FileSink c(base);
        a.receive({"from a", slog::LOG_INFO});
        b.receive({"from b", slog::LOG_INFO});
        c.receive({"from c", slog::LOG_INFO});
    }
    CHECK(readFile(base + ".log").find("from a") != std::string::npos);
    CHECK(readFile(base + "-1.log").find("from b") != std::string::npos);
    CHECK(readFile(base + "-2.log").find("from c") != std::string::npos);
}

TEST_CASE("FileSink attached to a Logger receives messages", "[logger]")
{
    test_helpers::TempDir tmp("logger");
    auto base = (tmp.path / "attached").string();
    {
        slog::Logger l;
        auto sink = std::make_shared<slog::FileSink>(base);
        // Ignore anything replayed from the init buffer
        sink->set_level(slog::LOG_CRIT);
        l.add_sink(sink);
        l.critical("attached %d", 99);
        l.error("below sink level");
    }
    std::string content = readFile(base + ".log");
    CHECK(content.find("(C) attached 99\n") != std::string::npos);
    CHECK(content.find("below sink level") == std::string::npos);
}

// A string literal format picks the const char * overloads, which carry the printf format
// attribute. They must format exactly like the std::string ones.
TEST_CASE("Logger formats a C string format like a std::string one", "[logger]")
{
    LoggerWithSink t;
    const char *cfmt = "%s=%d";
    std::string sfmt = "%s=%d";
    t.logger.info(cfmt, "a", 1);
    t.logger.info(sfmt, "a", 1);
    t.logger.notice("%zu %lld", (size_t)1 << 40, -3LL);
    t.logger.info(std::string("concatenated ") + "%d", 4);
    REQUIRE(t.sink->msgs.size() == 4);
    CHECK(t.sink->msgs[0].str == "a=1");
    CHECK(t.sink->msgs[1].str == "a=1");
    CHECK(t.sink->msgs[2].str == "1099511627776 -3");
    CHECK(t.sink->msgs[2].lvl == slog::LOG_NOTICE);
    CHECK(t.sink->msgs[3].str == "concatenated 4");
}

// The other tests here pass string literals, which now go to the const char * overloads. The
// std::string overloads still serve every "..." + x call, so they keep their own checks.
TEST_CASE("Logger std::string format overloads log at their own level", "[logger]")
{
    LoggerWithSink t;
    std::string fmt = "%d%%";
    t.logger.trace(fmt, 0);
    t.logger.debug(fmt, 1);
    t.logger.info(fmt, 2);
    t.logger.notice(fmt, 3);
    t.logger.warn(fmt, 4);
    t.logger.error(fmt, 5);
    t.logger.critical(fmt, 6);

    REQUIRE(t.sink->msgs.size() == 7);
    const slog::LogLevel expected[] = {slog::LOG_TRACE, slog::LOG_DEBUG, slog::LOG_INFO, slog::LOG_NOTICE, slog::LOG_WARN, slog::LOG_ERROR, slog::LOG_CRIT};
    for (int i = 0; i < 7; i++)
    {
        CHECK(t.sink->msgs[i].lvl == expected[i]);
        CHECK(t.sink->msgs[i].str == std::to_string(i) + "%");
    }
}

TEST_CASE("Logger std::string format keeps every character around 1024 characters", "[logger]")
{
    LoggerWithSink t;
    std::string fmt = "%s";
    for (size_t len : {1023, 1024, 1025})
    {
        std::string exact(len, 'x');
        t.logger.info(fmt, exact.c_str());
        t.logger.info("%s", exact.c_str());
    }
    REQUIRE(t.sink->msgs.size() == 6);
    for (int i = 0; i < 3; i++)
    {
        CHECK(t.sink->msgs[2 * i].str == std::string(1023 + i, 'x'));
        CHECK(t.sink->msgs[2 * i].str == t.sink->msgs[2 * i + 1].str);
    }
}
