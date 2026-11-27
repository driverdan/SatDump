#include "angelscript/angelscript.h"
#include "angelscript/scriptbuilder/scriptbuilder.h"
#include "angelscript/scriptsatdump/bind_satdump.h"
#include "angelscript/scriptsatdump/helper.h"
#include "cli_test_helpers.h"
#include "logger.h"
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace
{
    // Counts the messages the engine sends to the caller's callback
    void countingCallback(const asSMessageInfo *msg, void *param)
    {
        if (msg->type == asMSGTYPE_ERROR)
            (*(int *)param)++;
    }

    // Builds a script in a module of its own, returns the result of BuildModule()
    int buildScript(asIScriptEngine *engine, const std::string &code)
    {
        CScriptBuilder builder;
        REQUIRE(builder.StartNewModule(engine, "test") >= 0);
        REQUIRE(builder.AddSectionFromMemory("test", code.c_str()) >= 0);
        return builder.BuildModule();
    }
} // namespace

TEST_CASE("AngelScript registerAll registers every binding", "[angelscript]")
{
    cli_test::LogCapture log;

    asIScriptEngine *engine = asCreateScriptEngine();
    REQUIRE(engine != nullptr);

    // Every binding uses a native calling convention, which AS_MAX_PORTABILITY does not support
    INFO("AngelScript options: " << asGetLibraryOptions());
    CHECK(std::string(asGetLibraryOptions()).find("AS_MAX_PORTABILITY") == std::string::npos);

    CHECK(satdump::script::registerAll(engine));
    INFO(log.dump());
    CHECK_FALSE(log.contains("[AngelScript]", slog::LOG_ERROR));

    // A script that uses a binding of each kind (global function, object type, method) builds and runs
    REQUIRE(buildScript(engine, "void main()\n"
                                "{\n"
                                "    logger::info(\"ts \" + formatTimestamp(\"%Y\", 0));\n"
                                "    image::Image img(8, 16, 4, 1);\n"
                                "    logger::info(\"w \" + img.width());\n"
                                "}\n") >= 0);
    CHECK(satdump::script::runVoidMainFromModule(engine, "test"));
    CHECK(log.contains("[AngelScript] ts 1970", slog::LOG_INFO));
    CHECK(log.contains("[AngelScript] w 16", slog::LOG_INFO));

    engine->ShutDownAndRelease();
}

TEST_CASE("AngelScript registerAll logs failed registrations", "[angelscript]")
{
    cli_test::LogCapture log;

    asIScriptEngine *engine = asCreateScriptEngine();
    REQUIRE(engine != nullptr);

    // An image::Image without reference counting makes the image binding's ADDREF and RELEASE fail.
    // The add-ons are not made to fail, as they assert on their own errors in debug builds
    engine->SetDefaultNamespace("image");
    REQUIRE(engine->RegisterObjectType("Image", 0, asOBJ_REF | asOBJ_NOCOUNT) >= 0);
    engine->SetDefaultNamespace("");

    CHECK_FALSE(satdump::script::registerAll(engine));
    INFO(log.dump());
    CHECK(log.contains("registrations failed, scripts cannot be built", slog::LOG_ERROR));
    CHECK(log.contains("Failed in call to function 'RegisterObjectBehaviour' with 'Image' and 'void f()'", slog::LOG_ERROR));

    // The engine then refuses to build scripts
    CHECK(buildScript(engine, "void main() {}\n") == asINVALID_CONFIGURATION);

    engine->ShutDownAndRelease();
}

TEST_CASE("AngelScript registerAll keeps the caller's message callback", "[angelscript]")
{
    cli_test::LogCapture log;

    asIScriptEngine *engine = asCreateScriptEngine();
    REQUIRE(engine != nullptr);

    int errors = 0;
    REQUIRE(engine->SetMessageCallback(asFUNCTION(countingCallback), &errors, asCALL_CDECL) >= 0);
    REQUIRE(satdump::script::registerAll(engine));
    CHECK(errors == 0);

    // The script's error goes to the caller's callback again
    CHECK(buildScript(engine, "void main() { undefined_function(); }\n") < 0);
    CHECK(errors > 0);

    engine->ShutDownAndRelease();
}

TEST_CASE("AngelScript registerAll removes its callback if the caller had none", "[angelscript]")
{
    cli_test::LogCapture log;

    asIScriptEngine *engine = asCreateScriptEngine();
    REQUIRE(engine != nullptr);
    REQUIRE(satdump::script::registerAll(engine));

    CHECK(engine->GetMessageCallback(nullptr, nullptr, nullptr) == asNO_FUNCTION);

    engine->ShutDownAndRelease();
}
