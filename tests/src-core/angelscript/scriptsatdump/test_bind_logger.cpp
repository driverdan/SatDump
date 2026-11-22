#include "angelscript/angelscript.h"
#include "angelscript/scriptbuilder/scriptbuilder.h"
#include "angelscript/scriptsatdump/bind_satdump.h"
#include "angelscript/scriptsatdump/helper.h"
#include "angelscript/scriptstdstring/scriptstdstring.h"
#include "cli_test_helpers.h"
#include "logger.h"
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace
{
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
    // The bindings log through the global logger
    cli_test::LogCapture log;

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

    // Nothing else is logged, and each text is exactly the script's string after the prefix
    CHECK(log.dump() == "[AngelScript] t 100%s %n\n"
                        "[AngelScript] d %s%s%s%s\n"
                        "[AngelScript] i 50% done %d\n"
                        "[AngelScript] w %x %p %%\n"
                        "[AngelScript] e %s %n\n"
                        "[AngelScript] c %\n");

    CHECK(log.contains("[AngelScript] t ", slog::LOG_TRACE));
    CHECK(log.contains("[AngelScript] d ", slog::LOG_DEBUG));
    CHECK(log.contains("[AngelScript] i ", slog::LOG_INFO));
    CHECK(log.contains("[AngelScript] w ", slog::LOG_WARN));
    CHECK(log.contains("[AngelScript] e ", slog::LOG_ERROR));
    CHECK(log.contains("[AngelScript] c ", slog::LOG_CRIT));
}
