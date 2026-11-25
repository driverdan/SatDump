#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"
#include "imgui/imgui_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <string>

// Tests of the headless ImGui context used by the tests of the ImGui callers, and of the
// SatDump additions to src-core/imgui (imgui_stdlib.h, imconfig.h). They check that the
// assumptions the other tests make about Dear ImGui still hold after an update.

using imgui_test::HeadlessContext;
using imgui_test::HeadlessOptions;

TEST_CASE("HeadlessContext renders frames and restores the previous context", "[imgui][headless]")
{
    ImGuiContext *before = ImGui::GetCurrentContext();
    {
        HeadlessContext ctx;
        CHECK(ImGui::GetCurrentContext() == ctx.context());

        ctx.frames(2, []() { ImGui::Text("Hello %d", 42); });
        CHECK(ctx.frameCount() == 2);
        CHECK(ctx.lastVertexCount() > 0);

        // The font atlas was created through the texture requests
        bool has_texture = false;
        for (ImTextureData *tex : ImGui::GetPlatformIO().Textures)
            if (tex->Status == ImTextureStatus_OK && tex->TexID != ImTextureID_Invalid)
                has_texture = true;
        CHECK(has_texture);
        CHECK(ImGui::GetFontSize() > 0.0f);
    }
    CHECK(ImGui::GetCurrentContext() == before);
}

TEST_CASE("HeadlessContext creates the ImPlot, ImPlot3D and imnodes contexts", "[imgui][headless]")
{
    HeadlessOptions opts;
    opts.implot = opts.implot3d = opts.imnodes = true;
    HeadlessContext ctx(opts);
    CHECK(ImPlot::GetCurrentContext() != nullptr);
    CHECK(ImPlot3D::GetCurrentContext() != nullptr);

    float xs[3] = {0, 1, 2}, ys[3] = {1, 3, 2};
    ctx.frames(2,
               [&]()
               {
                   if (ImPlot::BeginPlot("##plot", ImVec2(300, 200)))
                   {
                       ImPlot::PlotLine("line", xs, ys, 3);
                       ImPlot::EndPlot();
                   }
                   if (ImPlot3D::BeginPlot("##plot3d", ImVec2(300, 200)))
                   {
                       ImPlot3D::PlotScatter("points", xs, ys, xs, 3);
                       ImPlot3D::EndPlot();
                   }
                   ImNodes::BeginNodeEditor();
                   ImNodes::BeginNode(1);
                   ImGui::TextUnformatted("node");
                   ImNodes::EndNode();
                   ImNodes::EndNodeEditor();
               });
    CHECK(ctx.lastVertexCount() > 0);
}

TEST_CASE("HeadlessContext clicks buttons with the mouse", "[imgui][headless]")
{
    HeadlessContext ctx;
    ImVec2 center;
    int clicks = 0;
    auto draw = [&]()
    {
        if (ImGui::Button("Press me"))
            clicks++;
        center = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
    };
    ctx.frame(draw);
    CHECK(clicks == 0);

    ctx.click(center, draw);
    CHECK(clicks == 1);

    // A click outside of the button does nothing
    ctx.click(ImVec2(1000, 600), draw);
    CHECK(clicks == 1);
}

TEST_CASE("imgui_stdlib InputText grows the std::string as text is typed", "[imgui][headless][imgui_stdlib]")
{
    HeadlessContext ctx;
    std::string text = "ab";
    ImVec2 center;
    bool deactivated_after_edit = false;
    auto draw = [&]()
    {
        ImGui::InputText("##text", &text);
        center = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
        if (ImGui::IsItemDeactivatedAfterEdit())
            deactivated_after_edit = true;
    };
    ctx.frame(draw);
    ctx.click(center, draw);
    CHECK(ImGui::IsAnyItemActive());

    // Longer than the initial capacity, so the resize callback has to run
    const std::string typed = "_0123456789abcdefghijklmnopqrstuvwxyz_0123456789abcdefghijklmnopqrstuvwxyz";
    ctx.pressKey(ImGuiKey_End, draw);
    ctx.typeText(typed);
    ctx.frame(draw);
    CHECK(text == "ab" + typed);
    CHECK_FALSE(deactivated_after_edit);

    ctx.pressKey(ImGuiKey_Enter, draw);
    CHECK(deactivated_after_edit);
    CHECK_FALSE(ImGui::IsAnyItemActive());
}

TEST_CASE("imgui_stdlib InputTextMultiline and InputTextWithHint use std::string", "[imgui][headless][imgui_stdlib]")
{
    HeadlessContext ctx;
    std::string multi = "line 1\nline 2", hint;
    ctx.frames(2,
               [&]()
               {
                   ImGui::InputTextMultiline("##multi", &multi, ImVec2(200, 100));
                   ImGui::InputTextWithHint("##hint", "a hint", &hint);
               });
    CHECK(multi == "line 1\nline 2");
    CHECK(hint.empty());
}

TEST_CASE("imconfig.h settings are in effect", "[imgui][headless]")
{
    // SatDump uses 32-bit indices so that large plots and images fit in one draw command
    STATIC_CHECK(sizeof(ImDrawIdx) == 4);
    // and the ImVec2 / ImVec4 operators
    ImVec2 v = ImVec2(1, 2) + ImVec2(3, 4) * 2.0f;
    CHECK(v.x == 7.0f);
    CHECK(v.y == 10.0f);
}
