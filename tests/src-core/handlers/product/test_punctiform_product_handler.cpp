#include "handlers/product/punctiform_product_handler.h"
#include "imgui/imgui_test_helpers.h"
#include "imgui/implot/implot_internal.h"
#include "pipeline/pipeline_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>

// The graph mode of PunctiformProductHandler, drawn in a headless ImGui context

using imgui_test::HeadlessContext;
using imgui_test::HeadlessOptions;
using namespace pipeline_test;
using satdump::handlers::PunctiformProductHandler;
using satdump::products::PunctiformProduct;

namespace
{
    std::shared_ptr<PunctiformProduct> makeProduct()
    {
        auto p = std::make_shared<PunctiformProduct>();
        p->instrument_name = "unit_instr_punctiform";
        PunctiformProduct::DataHolder ch;
        ch.channel_name = "1";
        for (int i = 0; i < 100; i++)
        {
            ch.timestamps.push_back(1221913540.0 + 10 * i);
            ch.data.push_back(i % 10);
        }
        p->data.push_back(ch);
        return p;
    }

    // Runs with the current directory set to an empty "resources" tree, so no instrument
    // configuration is loaded
    struct ResourcesEnv
    {
        TempDir dir;
        CwdGuard cwd;
        ResourcesEnv() : dir("handlers_punctiform"), cwd(dir.path) { std::filesystem::create_directories(dir.path / "resources" / "instrument_cfgs" / "common"); }
    };

    HeadlessOptions plotOptions()
    {
        HeadlessOptions opts;
        opts.implot = true;
        return opts;
    }
} // namespace

TEST_CASE("PunctiformProductHandler draws the selected channel as a graph", "[handlers][punctiform_product_handler]")
{
    ResourcesEnv env;
    HeadlessContext ctx(plotOptions());
    PunctiformProductHandler handler(makeProduct());
    REQUIRE(handler.current_mode == PunctiformProductHandler::MODE_GRAPH);
    CHECK(handler.selected_channel == "1");

    bool plot_open = true;
    ctx.frames(3,
               [&]()
               {
                   handler.drawContents(ImVec2(400, 300));
                   plot_open = ImPlot::GetCurrentContext()->CurrentPlot != nullptr;
               });
    CHECK_FALSE(plot_open); // EndPlot() was called
    CHECK(ctx.lastVertexCount() > 100);
}

TEST_CASE("PunctiformProductHandler skips EndPlot when BeginPlot fails", "[handlers][punctiform_product_handler]")
{
    // #416: drawContents called EndPlot() even when BeginPlot() returned false, which
    // asserts in ImPlot or dereferences its null current plot
    ResourcesEnv env;
    HeadlessContext ctx(plotOptions());
    PunctiformProductHandler handler(makeProduct());

    bool plot_open = true;
    ctx.frames(3,
               [&]()
               {
                   // Below the bottom of the window, so the plot is clipped and BeginPlot() returns false
                   ImGui::SetCursorPosY(ImGui::GetWindowHeight() + 1000);
                   handler.drawContents(ImVec2(400, 300));
                   plot_open = ImPlot::GetCurrentContext()->CurrentPlot != nullptr;
               });
    CHECK_FALSE(plot_open);

    // The plot still draws once it is visible again
    ctx.frames(2, [&]() { handler.drawContents(ImVec2(400, 300)); });
    CHECK(ctx.lastVertexCount() > 100);
}
