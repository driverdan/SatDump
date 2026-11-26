#include "cli_test_helpers.h"
#include "core/cli/cli.h"
#include "core/plugin.h"
#include "legacy.h"
#include <catch2/catch_test_macros.hpp>

// LegacyCmdHandler::run() ends with exit(), so only the registration of the "legacy"
// subcommand and the way it collects its arguments are tested.

using namespace cli_test;

namespace
{
    // Parses "satdump <args>" with an app holding only the legacy subcommand
    struct LegacyApp
    {
        CLI::App app{"test"};
        satdump::LegacyCmdHandler handler;

        LegacyApp() { handler.reg(&app); }

        void parse(std::vector<std::string> args)
        {
            args.insert(args.begin(), "satdump");
            Argv a(args);
            app.parse(a.argc(), a.argv());
        }

        CLI::App *sub() { return app.get_subcommand("legacy"); }
    };
} // namespace

TEST_CASE("LegacyCmdHandler registers the legacy subcommand", "[cli][legacy]")
{
    LegacyApp l;

    CHECK(l.handler.cmd == "legacy");
    REQUIRE(l.app.get_subcommands([](CLI::App *) { return true; }).size() == 1);
    CHECK(l.sub()->get_name() == "legacy");
    CHECK_FALSE(l.sub()->get_description().empty());
    CHECK_FALSE(l.sub()->parsed());
}

TEST_CASE("LegacyCmdHandler passes everything after legacy through untouched", "[cli][legacy]")
{
    LegacyApp l;

    SECTION("positionals and flags stay in order")
    {
        l.parse({"legacy", "metop_ahrpt", "baseband", "in.cs16", "out", "--samplerate", "6e6", "--dc_block"});
        CHECK(l.sub()->parsed());
        CHECK(l.sub()->remaining() == std::vector<std::string>{"metop_ahrpt", "baseband", "in.cs16", "out", "--samplerate", "6e6", "--dc_block"});
    }

    SECTION("no arguments at all")
    {
        l.parse({"legacy"});
        CHECK(l.sub()->parsed());
        CHECK(l.sub()->remaining().empty());
    }

    SECTION("arguments starting with a flag")
    {
        l.parse({"legacy", "--v"});
        CHECK(l.sub()->remaining() == std::vector<std::string>{"--v"});
    }

    SECTION("help flags are left to the legacy command")
    {
        // The subcommand has no help flag of its own, so -h is handed over like any other
        // argument instead of raising CLI::CallForHelp
        CHECK_NOTHROW(l.parse({"legacy", "-h"}));
        CHECK(l.sub()->remaining() == std::vector<std::string>{"-h"});
        CHECK(l.sub()->get_help_ptr() == nullptr);
    }

    SECTION("a name that is also a top-level word is not interpreted")
    {
        l.parse({"legacy", "live", "legacy", "--help"});
        CHECK(l.sub()->remaining() == std::vector<std::string>{"live", "legacy", "--help"});
    }
}

TEST_CASE("LegacyCmdHandler forwards the arguments after legacy, without a leading --", "[cli][legacy]")
{
    LegacyApp l;
    auto fwd = [&l]() { return satdump::LegacyCmdHandler::get_forwarded_args(l.sub()); };

    SECTION("ordinary command lines are forwarded as they are")
    {
        l.parse({"legacy", "live", "goes_hrit", "out", "--source", "file", "--samplerate", "2.4e6", "--fill_missing", "--tracked_addresses", "AAAA0001,AAAA0002"});
        CHECK(fwd() == std::vector<std::string>{"live", "goes_hrit", "out", "--source", "file", "--samplerate", "2.4e6", "--fill_missing", "--tracked_addresses", "AAAA0001,AAAA0002"});
    }

    SECTION("a leading -- is dropped, so legacy -- live is legacy live")
    {
        // CLI11 >= 2.6.2 keeps the -- in remaining(); the legacy dispatcher would take it for a pipeline id
        l.parse({"legacy", "--", "live", "goes_hrit", "out", "--source", "file"});
        CHECK(l.sub()->remaining() == std::vector<std::string>{"--", "live", "goes_hrit", "out", "--source", "file"});
        CHECK(fwd() == std::vector<std::string>{"live", "goes_hrit", "out", "--source", "file"});
    }

    SECTION("only one leading -- is dropped")
    {
        l.parse({"legacy", "--", "--", "version"});
        CHECK(fwd() == std::vector<std::string>{"--", "version"});
    }

    SECTION("a lone -- forwards nothing")
    {
        l.parse({"legacy", "--"});
        CHECK(fwd().empty());
    }

    SECTION("a -- later in the line is passed through")
    {
        l.parse({"legacy", "live", "goes_hrit", "out", "--", "--x", "1"});
        CHECK(fwd() == std::vector<std::string>{"live", "goes_hrit", "out", "--", "--x", "1"});
    }

    SECTION("no arguments forward nothing")
    {
        l.parse({"legacy"});
        CHECK(fwd().empty());
    }
}

TEST_CASE("LegacyCmdHandler plugs into the CommandHandler like the satdump executable does", "[cli][legacy]")
{
    // Same registration as src-cli/main.cpp. reg() of the built-in commands reads (and with
    // operator[] may add to) the global config.
    EventBusGuard bus;
    ConfigGuard cfg;
    satdump::eventBus->register_handler<satdump::cli::RegisterSubcommandEvent>([](const satdump::cli::RegisterSubcommandEvent &evt)
                                                                               { evt.cmd_handlers.push_back(std::make_shared<satdump::LegacyCmdHandler>()); });

    satdump::cli::CommandHandler h;

    SECTION("legacy command lines are accepted")
    {
        Argv a({"satdump", "legacy", "record", "out", "--source", "file", "--samplerate", "1e6"});
        CHECK(h.parse(a.argc(), a.argv()) == 0);
        CHECK(h.has_command());
    }

    SECTION("legacy help is a plain argument")
    {
        Argv a({"satdump", "legacy", "help"});
        CHECK(h.parse(a.argc(), a.argv()) == 0);
        CHECK(h.has_command());
    }

    SECTION("legacy -h is a plain argument")
    {
        // -h is handed to the legacy command, so CommandHandler::run() would run it (#228).
        // LegacyCmdHandler::run() ends with exit(), so run() must not be called here: that
        // -h reaches a prefix command is tested in test_cli.cpp with a test-only command.
        Argv a({"satdump", "legacy", "-h"});
        CHECK(h.parse(a.argc(), a.argv()) == 0);
        CHECK(h.has_command());
    }

    SECTION("--help-all after legacy is handled by CLI11, and the command is then not run")
    {
        // --help-all belongs to the top-level app, so CLI11 prints the help and run() does
        // nothing. If it did run the command, this would exit the test executable.
        Argv a({"satdump", "legacy", "--help-all"});
        CHECK(h.parse(a.argc(), a.argv()) == 0);
        CHECK_FALSE(h.run());
    }
}
