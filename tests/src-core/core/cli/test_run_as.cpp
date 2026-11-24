#include "core/cli/cli.h"
#include "core/cli/run_as.h"
#include "helpers/satdump_guards.h"
#include "helpers/temp_files.h"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

// `satdump run` builds and runs an AngelScript file. Scripts that fail must be reported (#87), as
// satdump's exit status comes from CommandHandler::run()

namespace
{
    // Parses and runs `satdump run <args>`, and returns what CommandHandler::run() returns (true on error)
    bool runCommand(std::vector<std::string> args)
    {
        test_helpers::ConfigGuard cg;
        args.insert(args.begin(), {"satdump", "run"});
        std::vector<char *> argv;
        for (auto &a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);

        satdump::cli::CommandHandler h(false);
        REQUIRE(h.parse((int)args.size(), argv.data()) == 0);
        return h.run();
    }
} // namespace

TEST_CASE("ScriptCmdHandler::runAngelScript reports a script that fails", "[core][cli][angelscript]")
{
    satdump::ScriptCmdHandler handler;

    SECTION("a valid script")
    {
        test_helpers::TempFile f("good.as", "void main() {}\n");
        CHECK(handler.runAngelScript(f.str(), false, false) == 0);
    }
    SECTION("a script that does not build")
    {
        test_helpers::TempFile f("bad.as", "void main() { int x = ; }\n");
        CHECK(handler.runAngelScript(f.str(), false, false) != 0);
    }
    SECTION("a script without main()")
    {
        test_helpers::TempFile f("nomain.as", "void other() {}\n");
        CHECK(handler.runAngelScript(f.str(), false, false) != 0);
    }
    SECTION("a script whose main() raises an exception")
    {
        test_helpers::TempFile f("throws.as", "void main() { int z = 0; int x = 1 / z; }\n");
        CHECK(handler.runAngelScript(f.str(), false, false) != 0);
    }
    SECTION("a missing script")
    {
        test_helpers::TempFile f("missing.as");
        CHECK(handler.runAngelScript(f.str(), false, false) != 0);
    }
}

TEST_CASE("satdump run reports a script that fails (#87)", "[core][cli][angelscript]")
{
    test_helpers::TempFile good("good.as", "void main() {}\n");
    test_helpers::TempFile bad("bad.as", "void main() { int x = ; }\n");
    test_helpers::TempFile throws("throws.as", "void main() { int z = 0; int x = 1 / z; }\n");

    CHECK_FALSE(runCommand({good.str()}));
    CHECK(runCommand({bad.str()}));
    CHECK(runCommand({throws.str()}));

    // Linting only prints the diagnostics, whatever they are
    CHECK_FALSE(runCommand({good.str(), "--lint"}));
    CHECK_FALSE(runCommand({bad.str(), "--lint"}));
}
