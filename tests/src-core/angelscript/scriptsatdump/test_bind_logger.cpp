#include "angelscript/angelscript.h"
#include "angelscript/scriptbuilder/scriptbuilder.h"
#include "angelscript/scriptsatdump/bind_satdump.h"
#include "angelscript/scriptsatdump/helper.h"
#include "angelscript/scriptstdstring/scriptstdstring.h"
#include "logger.h"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // Replaces the global logger with one that only records, and restores the previous one on
    // destruction. The bindings log through the global logger.
    class LogCapture
    {
    private:
        class Sink : public slog::LoggerSink
        {
        public:
            std::mutex mtx;
            std::vector<slog::LogMsg> msgs;

            void receive(slog::LogMsg log)
            {
                std::lock_guard<std::mutex> l(mtx);
                msgs.push_back(log);
            }
        };

        std::shared_ptr<slog::Logger> saved;
        std::shared_ptr<Sink> sink = std::make_shared<Sink>();

    public:
        LogCapture() : saved(logger)
        {
            logger = std::make_shared<slog::Logger>();
            logger->add_sink(sink);
            // add_sink replays the messages logged before completeLoggerInit(), which the test
            // main never calls
            std::lock_guard<std::mutex> l(sink->mtx);
            sink->msgs.clear();
        }

        ~LogCapture() { logger = saved; }

        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        std::vector<slog::LogMsg> messages()
        {
            std::lock_guard<std::mutex> l(sink->mtx);
            return sink->msgs;
        }
    };

    // Builds a script that only uses the string type and the logger binding, and runs its main()
    void runScript(const std::string &code)
    {
        asIScriptEngine *engine = asCreateScriptEngine();
        REQUIRE(engine != nullptr);
        RegisterStdString(engine);
        satdump::script::registerLogger(engine);

        CScriptBuilder builder;
        REQUIRE(builder.StartNewModule(engine, "test") >= 0);
        REQUIRE(builder.AddSectionFromMemory("test", code.c_str()) >= 0);
        REQUIRE(builder.BuildModule() >= 0);
        REQUIRE(satdump::script::runVoidMainFromModule(engine, "test"));

        engine->ShutDownAndRelease();
    }
} // namespace

TEST_CASE("AngelScript logger binding logs the script's text literally", "[angelscript][logger]")
{
    LogCapture capture;

    // %n would make vsnprintf write through a pointer it was never given, and %s read one
    runScript("void main()\n"
              "{\n"
              "    logger::trace(\"t 100%s %n\");\n"
              "    logger::debug(\"d %s%s%s%s\");\n"
              "    logger::info(\"i 50% done %d\");\n"
              "    logger::warn(\"w %x %p %%\");\n"
              "    logger::error(\"e %s %n\");\n"
              "    logger::critical(\"c %\");\n"
              "}\n");

    std::vector<slog::LogMsg> msgs = capture.messages();
    REQUIRE(msgs.size() == 6);

    CHECK(msgs[0].lvl == slog::LOG_TRACE);
    CHECK(msgs[0].str == "[AngelScript] t 100%s %n");
    CHECK(msgs[1].lvl == slog::LOG_DEBUG);
    CHECK(msgs[1].str == "[AngelScript] d %s%s%s%s");
    CHECK(msgs[2].lvl == slog::LOG_INFO);
    CHECK(msgs[2].str == "[AngelScript] i 50% done %d");
    CHECK(msgs[3].lvl == slog::LOG_WARN);
    CHECK(msgs[3].str == "[AngelScript] w %x %p %%");
    CHECK(msgs[4].lvl == slog::LOG_ERROR);
    CHECK(msgs[4].str == "[AngelScript] e %s %n");
    CHECK(msgs[5].lvl == slog::LOG_CRIT);
    CHECK(msgs[5].str == "[AngelScript] c %");
}
