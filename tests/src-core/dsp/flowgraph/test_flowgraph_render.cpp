#include "core/style.h"
#include "dsp/flowgraph/dsp_flowgraph_register.h"
#include "dsp/flowgraph/flowgraph.h"
#include "dsp/io/file_sink.h"
#include "dsp/io/file_source.h"
#include "dsp/utils/add.h"
#include "dsp/utils/multiply_const.h"
#include "helpers/guards.h"
#include "imgui/imgui_test_helpers.h"
#include "imgui/imnodes/imnodes_internal.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Flowgraph::render() draws the flowgraph with imnodes, here in a headless ImGui context

using Catch::Matchers::WithinAbs;
using imgui_test::HeadlessContext;
using imgui_test::HeadlessOptions;
using namespace satdump::ndsp;
using namespace satdump::ndsp::flowgraph;

namespace
{
    struct RenderEnv
    {
        test_helpers::ValueGuard<float> scale_guard{ui_scale, 1.0f};
        HeadlessContext ctx;

        static HeadlessOptions options()
        {
            HeadlessOptions o;
            o.imnodes = true;
            return o;
        }

        RenderEnv() : ctx(options()) {}

        std::function<void()> drawer(Flowgraph &fg)
        {
            return [&fg]() { fg.render(); };
        }
    };

    // A file source, a multiply const and a file sink, at the given grid positions
    void makeChain(Flowgraph &fg, bool linked)
    {
        registerNodeSimple<FileSourceBlock<float>>(fg, "IO/File Source F");
        registerNodeSimple<FileSinkBlock<float>>(fg, "IO/File Sink F");
        registerNodeSimple<MultiplyConstBlock<float>>(fg, "Utils/Multiply Const FF");
        for (const char *id : {"file_source_f", "multiply_constf", "file_sink_f"})
            fg.addNode(id, fg.node_internal_registry.at(id).inst(&fg));

        nlohmann::json j = fg.getJSON();
        for (int i = 0; i < 3; i++)
        {
            j["nodes"][i]["pos_x"] = 20.0f + 350.0f * i;
            j["nodes"][i]["pos_y"] = 40.0f + 10.0f * i;
        }
        if (linked)
        {
            int src_out = j["nodes"][0]["io"][0]["id"];
            int mul_in = j["nodes"][1]["io"][0]["id"];
            j["links"] = {{{"id", 0}, {"start", src_out}, {"end", mul_in}}};
        }
        fg.setJSON(j);
    }

#ifndef _MSC_VER // GImNodes is not exported from the Windows DLL
    // Screen position of a pin, as computed by imnodes for the last frame
    ImVec2 pinPos(int pin_id)
    {
        ImNodesEditorContext &editor = ImNodes::EditorContextGet();
        int idx = ImNodes::ObjectPoolFind(editor.Pins, pin_id);
        REQUIRE(idx >= 0);
        return editor.Pins.Pool[idx].Pos;
    }
#endif
} // namespace

TEST_CASE("ndsp Flowgraph render keeps the node positions and links", "[dsp][flowgraph][render]")
{
    RenderEnv env;
    Flowgraph fg;
    makeChain(fg, true);
    nlohmann::json before = fg.getJSON();

    env.ctx.frames(3, env.drawer(fg));
    CHECK(env.ctx.lastVertexCount() > 0);

    nlohmann::json after = fg.getJSON();
    REQUIRE(after["nodes"].size() == 3);
    for (int i = 0; i < 3; i++)
    {
        CAPTURE(i);
        CHECK_THAT(after["nodes"][i]["pos_x"].get<float>(), WithinAbs(20.0f + 350.0f * i, 1e-3));
        CHECK_THAT(after["nodes"][i]["pos_y"].get<float>(), WithinAbs(40.0f + 10.0f * i, 1e-3));
    }
    CHECK(after["links"] == before["links"]);

    // imnodes knows the nodes by their flowgraph ids, at their grid positions
    for (auto &n : after["nodes"])
    {
        ImVec2 pos = ImNodes::GetNodeGridSpacePos(n["id"].get<int>());
        CHECK_THAT(pos.x, WithinAbs(n["pos_x"].get<float>(), 1e-3));
        CHECK(ImNodes::GetNodeDimensions(n["id"].get<int>()).x > 0);
    }
}

TEST_CASE("ndsp Flowgraph render deletes and disables the selected node", "[dsp][flowgraph][render]")
{
    RenderEnv env;
    Flowgraph fg;
    makeChain(fg, true);
    auto draw = env.drawer(fg);
    env.ctx.frames(2, draw);

    // Select the source by clicking on its title bar
    const int src_id = fg.getJSON()["nodes"][0]["id"];
    ImVec2 title = ImNodes::GetNodeScreenSpacePos(src_id) + ImVec2(10, 5);
    env.ctx.click(title, draw);
    REQUIRE(ImNodes::NumSelectedNodes() == 1);

    env.ctx.pressKey(ImGuiKey_D, draw);
    CHECK(fg.getJSON()["nodes"][0]["disabled"] == true);
    env.ctx.pressKey(ImGuiKey_D, draw);
    CHECK(fg.getJSON()["nodes"][0]["disabled"] == false);

    // Deleting it also deletes its link. getJSON() stores the nodes at the index of their id, so
    // the deleted node leaves a null entry.
    env.ctx.pressKey(ImGuiKey_Delete, draw);
    nlohmann::json j = fg.getJSON();
    REQUIRE(j["nodes"].size() == 3);
    CHECK(j["nodes"][src_id].is_null());
    CHECK(j["links"].empty());

    // and the remaining nodes are still drawn
    env.ctx.frames(2, draw);
    CHECK(fg.getJSON()["nodes"].size() == 3);
    CHECK(env.ctx.lastVertexCount() > 0);
}

#ifndef _MSC_VER
TEST_CASE("ndsp Flowgraph render creates a link dragged from an output to an input", "[dsp][flowgraph][render]")
{
    RenderEnv env;
    Flowgraph fg;
    makeChain(fg, false);
    auto draw = env.drawer(fg);
    env.ctx.frames(2, draw);
    REQUIRE(fg.getJSON()["links"].empty());

    nlohmann::json j = fg.getJSON();
    const int mul_out = j["nodes"][1]["io"][1]["id"];
    const int sink_in = j["nodes"][2]["io"][0]["id"];
    REQUIRE(j["nodes"][1]["io"][1]["is_out"] == true);
    const ImVec2 from = pinPos(mul_out), to = pinPos(sink_in);

    env.ctx.mouseMove(from);
    env.ctx.frame(draw);
    env.ctx.mouseButton(ImGuiMouseButton_Left, true);
    env.ctx.frame(draw);
    for (int i = 1; i <= 5; i++)
    {
        env.ctx.mouseMove(from + (to - from) * (i / 5.0f));
        env.ctx.frame(draw);
    }
    env.ctx.mouseButton(ImGuiMouseButton_Left, false);
    env.ctx.frame(draw);
    env.ctx.frame(draw);

    j = fg.getJSON();
    REQUIRE(j["links"].size() == 1);
    CHECK(j["links"][0]["start"] == mul_out);
    CHECK(j["links"][0]["end"] == sink_in);
}

// Regression (#164): setJSON() refreshed the IO types from the block with the input index
// incremented twice, so every input after the first kept the type it was loaded with (the
// type is not saved in the JSON) and its pin was drawn in the wrong color.
TEST_CASE("ndsp Flowgraph render colors every input pin of a loaded node by its type", "[dsp][flowgraph][render]")
{
    RenderEnv env;
    Flowgraph fg;
    registerNodeSimple<AddBlock<float>>(fg, "Utils/Add FF");
    fg.addNode("add_f", fg.node_internal_registry.at("add_f").inst(&fg));
    fg.setJSON(fg.getJSON());
    env.ctx.frames(2, env.drawer(fg));

    nlohmann::json j = fg.getJSON();
    REQUIRE(j["nodes"][0]["io"].size() == 3);
    const ImU32 f32 = getColorFromDSPType(DSP_SAMPLE_TYPE_F32);
    ImNodesEditorContext &editor = ImNodes::EditorContextGet();
    for (auto &io : j["nodes"][0]["io"])
    {
        CAPTURE(io["name"].get<std::string>());
        int idx = ImNodes::ObjectPoolFind(editor.Pins, io["id"].get<int>());
        REQUIRE(idx >= 0);
        CHECK(editor.Pins.Pool[idx].ColorStyle.Background == f32);
    }
}
#endif

TEST_CASE("ndsp Flowgraph renderAddMenuList adds the clicked node", "[dsp][flowgraph][render]")
{
    RenderEnv env;
    Flowgraph fg;
    registerNodeSimple<MultiplyConstBlock<float>>(fg, "Utils/Multiply Const FF");
    registerNodeSimple<FileSinkBlock<float>>(fg, "IO/File Sink F");

    // Searching opens every category, so the leaves are visible
    ImVec2 leaf;
    bool found = false;
    auto draw = [&]()
    {
        fg.renderAddMenuList("Multiply");
        found = ImGui::GetItemRectSize().x > 0;
        leaf = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
    };
    env.ctx.frames(2, draw);
    REQUIRE(found);
    env.ctx.click(leaf, draw);

    nlohmann::json j = fg.getJSON();
    REQUIRE(j["nodes"].size() == 1);
    CHECK(j["nodes"][0]["int_id"] == "multiply_constf");
}

// Regression (#420): the "Add others" list of the variables window looped over a temporary
// get_cfg(), which was destroyed before the loop ran. In Debug builds the loop never ended, so
// a regression shows up as this test hitting its CTest timeout.
TEST_CASE("ndsp Flowgraph renderWindows lists the block parameters to assign from variables", "[dsp][flowgraph][render]")
{
    RenderEnv env;
    Flowgraph fg;
    makeChain(fg, false);
    auto draw = [&fg]()
    {
        fg.render();
        fg.renderWindows();
    };
    env.ctx.frames(2, draw);

    // Double-clicking the multiply const opens its variables window
    const int mul_id = fg.getJSON()["nodes"][1]["id"];
    ImVec2 title = ImNodes::GetNodeScreenSpacePos(mul_id) + ImVec2(10, 5);
    env.ctx.click(title, draw);
    env.ctx.click(title, draw);
    REQUIRE_NOTHROW(env.ctx.frames(2, draw));
    ImGuiWindow *win = ImGui::FindWindowByName("multiply_constf##nodevarspopup");
    REQUIRE(win != nullptr);
    REQUIRE(win->Active);

    // Its only parameter, "const", is the last button, at the bottom left of the auto-resized window
    REQUIRE(fg.getJSON()["nodes"][1]["vars"].empty());
    const ImGuiStyle &style = ImGui::GetStyle();
    ImVec2 button(win->Pos.x + style.WindowPadding.x + 5, win->Pos.y + win->Size.y - style.WindowPadding.y - ImGui::GetFrameHeight() / 2);
    env.ctx.click(button, draw);
    nlohmann::json vars = fg.getJSON()["nodes"][1]["vars"];
    REQUIRE(vars.size() == 1);
    CHECK(vars.contains("const"));
}
