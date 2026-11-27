#include "core/exception.h"
#include "handlers/product/image_product_handler.h"
#include "pipeline/pipeline_test_helpers.h"
#include "products/image_product.h"
#include "products/product_process.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

using namespace pipeline_test;
using satdump::products::ImageProduct;

namespace
{
    // Runs with the current directory set to a temporary folder holding a "resources" tree,
    // which takes precedence over the installed resources.
    struct ResourcesEnv
    {
        TempDir dir;
        CwdGuard cwd;
        ResourcesEnv() : dir("products_process"), cwd(dir.path) { std::filesystem::create_directories(dir.path / "resources" / "instrument_cfgs" / "common"); }

        void write(const std::string &rel, const std::string &content) { writeFile((dir.path / "resources" / rel).string(), content); }
    };

    // Shaped like the noaa_apt output when no AVHRR channel could be identified:
    // only the raw APT-A (abs_index -1) and APT-B (abs_index -2) channels, no
    // calibration and no projection.
    std::shared_ptr<ImageProduct> makeUnidentifiedAptProduct()
    {
        auto p = std::make_shared<ImageProduct>();
        p->instrument_name = "unit_apt";
        for (auto [abs_index, name] : std::vector<std::pair<int, std::string>>{{-1, "a"}, {-2, "b"}})
        {
            ImageProduct::ImageHolder h;
            h.abs_index = abs_index;
            h.channel_name = name;
            h.filename = "APT-" + name;
            h.image = satdump::image::Image(8, 20, 10, 1);
            h.image.fill(100);
            h.bit_depth = 8;
            p->images.push_back(h);
        }
        return p;
    }

    void setOutputConfig(const std::string &out)
    {
        satdump::satdump_cfg.main_cfg["satdump_directories"]["default_image_output_directory"]["value"] = out;
        satdump::satdump_cfg.main_cfg["satdump_directories"]["image_filename_template"]["value"] = "$c";
        satdump::satdump_cfg.main_cfg["satdump_general"]["image_format"]["value"] = "png";
    }
} // namespace

TEST_CASE("ImageProductHandler accepts a product without projection whose channel has abs_index -1", "[products][product_process]")
{
    ResourcesEnv env;
    ConfigGuard cfg;
    setOutputConfig(env.dir.str());

    auto prod = makeUnidentifiedAptProduct();
    REQUIRE_FALSE(prod->has_proj_cfg());

    // The sun angle converter set up for channel "a" used to throw "Image width must be present!"
    std::shared_ptr<satdump::handlers::ImageProductHandler> h;
    REQUIRE_NOTHROW(h = std::make_shared<satdump::handlers::ImageProductHandler>(prod));
    CHECK_FALSE(prod->has_proj_cfg());
}

TEST_CASE("process_product_with_handler generates the presets of a product without projection", "[products][product_process]")
{
    ResourcesEnv env;
    ConfigGuard cfg;
    TempDir out("products_process_out");
    setOutputConfig(out.str());
    env.write("instrument_cfgs/unit_apt.json", R"({
        "name": "Unit APT",
        "presets": [
            { "name": "Bad", "expression": "ch9", "autogen": true },
            { "name": "APT A", "expression": "cha", "autogen": true },
            { "name": "APT B", "expression": "chb", "autogen": true }
        ]
    })");

    auto prod = makeUnidentifiedAptProduct();
    REQUIRE_NOTHROW(satdump::products::process_product_with_handler(prod, out.str()));

    // The invalid preset fails on its own, the others are still saved
    CHECK(std::filesystem::exists(out.file("APT_A.png")));
    CHECK(std::filesystem::exists(out.file("APT_B.png")));
    CHECK_FALSE(prod->has_proj_cfg());
}

TEST_CASE("process_product_with_handler fails on a product without a handler", "[products][product_process]")
{
    ResourcesEnv env;
    EventBusGuard bus;
    TempDir out("products_process_out");

    auto prod = std::make_shared<satdump::products::Product>();
    prod->instrument_name = "unit_instr";
    prod->type = "unit_no_handler_type";
    // A real setup failure must stay visible to the caller
    CHECK_THROWS_AS(satdump::products::process_product_with_handler(prod, out.str()), satdump::satdump_exception_t);
}
