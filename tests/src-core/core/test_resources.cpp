#include "cli_test_helpers.h"
#include "core/resources.h"
#include "helpers/guards.h"
#include "helpers/temp_files.h"
#include "satdump_vars.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

// Resource lookup depends on the current directory and satdump::RESPATH. Both are changed only
// through RAII guards that restore them.

namespace
{
    using test_helpers::CwdGuard;
    using test_helpers::TempDir;
    using test_helpers::writeFile;
} // namespace

TEST_CASE("resources prefer a resources folder in the current directory", "[core][resources]")
{
    TempDir cwd("cwd");
    writeFile(cwd.path / "resources" / "maps" / "local.json", "x");
    TempDir respath("respath");
    writeFile(respath.path / "resources" / "maps" / "system.json", "x");

    CwdGuard cg(cwd.path);
    test_helpers::ValueGuard rg(satdump::RESPATH, respath.path.string() + "/");

    CHECK(resources::resourceExists("maps/local.json"));
    CHECK(resources::resourceExists("maps"));
    // The system location is not consulted at all while ./resources exists
    CHECK_FALSE(resources::resourceExists("maps/system.json"));
    CHECK(resources::getResourcePath("maps/local.json") == "resources/maps/local.json");
    // Missing resources still get a path (and an error is logged)
    CHECK(resources::getResourcePath("maps/system.json") == "resources/maps/system.json");
}

TEST_CASE("resources fall back to RESPATH without a local resources folder", "[core][resources]")
{
    TempDir cwd("empty_cwd");
    TempDir respath("respath");
    writeFile(respath.path / "resources" / "fonts" / "font.ttf", "x");

    CwdGuard cg(cwd.path);
    test_helpers::ValueGuard rg(satdump::RESPATH, respath.path.string() + "/");

    CHECK(resources::resourceExists("fonts/font.ttf"));
    CHECK_FALSE(resources::resourceExists("fonts/missing.ttf"));
    CHECK(resources::getResourcePath("fonts/font.ttf") == respath.path.string() + "/resources/fonts/font.ttf");
    CHECK(resources::getResourcePath("fonts/missing.ttf") == respath.path.string() + "/resources/fonts/missing.ttf");
}

TEST_CASE("resources resolve the repository's shipped resources", "[core][resources]")
{
    TempDir cwd("empty_cwd2");
    CwdGuard cg(cwd.path);
    test_helpers::ValueGuard rg(satdump::RESPATH, std::string(SATDUMP_TEST_SOURCE_DIR) + "/");

    CHECK(resources::resourceExists("themes"));
    CHECK(std::filesystem::is_directory(resources::getResourcePath("themes")));
}

// The resource name used to be part of the logger's format, so a '%' in it was interpreted:
// "%%" was logged as "%" (and "%s" or "%n" would read arguments that were never passed)
TEST_CASE("resources log the name of a missing resource as written", "[core][resources]")
{
    TempDir cwd("cwd_percent");
    std::filesystem::create_directories(cwd.path / "resources");
    TempDir respath("respath_percent");

    CwdGuard cg(cwd.path);
    test_helpers::ValueGuard rg(satdump::RESPATH, respath.path.string() + "/");

    {
        cli_test::LogCapture log;
        CHECK(resources::getResourcePath("maps/100%%.json") == "resources/maps/100%%.json");
        INFO(log.dump());
        CHECK(log.contains("Resources maps/100%%.json does not exist!", slog::LOG_ERROR));
    }

    // Without a local resources folder, the RESPATH branch
    std::filesystem::remove_all(cwd.path / "resources");
    {
        cli_test::LogCapture log;
        resources::getResourcePath("fonts/50%%.ttf");
        INFO(log.dump());
        CHECK(log.contains("Resources fonts/50%%.ttf does not exist!", slog::LOG_ERROR));
    }
}
