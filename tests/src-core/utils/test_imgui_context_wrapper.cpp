#include "core/backend.h"
#include "helpers/guards.h"
#include "helpers/temp_files.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_test_helpers.h"
#include "utils/imgui_context_wrapper.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// ContainedContext runs a second ImGui context inside a child window of the current one (the
// zoomable flowgraph editors use it). It relies on ImGui internals: the input event trail, the
// draw lists and the shared font atlas.

using Catch::Matchers::WithinAbs;
using imgui_test::HeadlessContext;

namespace
{
    // Run in a temporary directory, so a stray imgui.ini (#423) can be detected without touching
    // the real working directory. The mouse offsets are restored too.
    struct WrapperEnv
    {
        test_helpers::TempDir dir{"imgui_context_wrapper"};
        test_helpers::CwdGuard cwd{dir.path};
        test_helpers::ValueGuard<float> ox{backend::mouse_set_offset_x, 0.0f};
        test_helpers::ValueGuard<float> oy{backend::mouse_set_offset_y, 0.0f};
    };

    struct Inner
    {
        ImVec2 button_min, button_max; // In the coordinates of the contained context
        int clicks = 0;
        ImGuiContext *ctx = nullptr;
    };

    void drawContained(ContainedContext &cc, Inner &inner)
    {
        cc.begin();
        inner.ctx = ImGui::GetCurrentContext();
        if (ImGui::Button("Inner button", ImVec2(100, 40)))
            inner.clicks++;
        inner.button_min = ImGui::GetItemRectMin();
        inner.button_max = ImGui::GetItemRectMax();
        cc.end();
    }
} // namespace

TEST_CASE("ContainedContext draws a second context into the current window", "[utils][imgui_context_wrapper]")
{
    WrapperEnv env;
    HeadlessContext ctx;
    ContainedContext cc;
    cc.config().extra_window_wrapper = true;
    cc.config().size = ImVec2(600, 400);

    Inner inner;
    ImGuiContext *outer_during_frame = nullptr;
    auto draw = [&]()
    {
        drawContained(cc, inner);
        outer_during_frame = ImGui::GetCurrentContext();
    };
    ctx.frames(2, draw);

    CHECK(inner.ctx == cc.getRawContext());
    CHECK(inner.ctx != ctx.context());
    CHECK(outer_during_frame == ctx.context()); // end() switched back
    CHECK(cc.scale() == 1.0f);
    CHECK(cc.size().x > 0);
    CHECK(backend::mouse_set_offset_x == 0.0f); // Restored by end()

    // The contained context's vertices are appended to the outer draw data
    CHECK(ctx.lastVertexCount() > 0);

    // Clicking on the button in outer coordinates clicks it in the contained context
    const ImVec2 outer_center = (inner.button_min + inner.button_max) * 0.5f * cc.scale() + cc.origin();
    ctx.click(outer_center, draw);
    CHECK(inner.clicks == 1);
    CHECK(cc.hovered());

    // A click elsewhere does not
    ctx.click(outer_center + ImVec2(200, 150), draw);
    CHECK(inner.clicks == 1);
}

TEST_CASE("ContainedContext maps the mouse through its zoom", "[utils][imgui_context_wrapper]")
{
    WrapperEnv env;
    HeadlessContext ctx;
    ContainedContext cc;
    cc.config().extra_window_wrapper = true;
    cc.config().size = ImVec2(600, 400);
    cc.config().zoom_smoothness = 0.0f; // Immediate
    cc.config().zoom_max = 3.0f;

    Inner inner;
    auto draw = [&]() { drawContained(cc, inner); };
    ctx.frame(draw);
    // Zoom in to 2x with the wheel, over the bottom right part of the view, away from the button
    ctx.mouseMove(cc.origin() + ImVec2(500, 300));
    ctx.frame(draw);
    for (int i = 0; i < 10; i++)
    {
        ctx.mouseWheel(1.0f);
        ctx.frame(draw);
    }
    ctx.frames(2, draw);
    REQUIRE_THAT(cc.scale(), WithinAbs(2.0, 1e-5));
    // The contained display is the visible area divided by the zoom
    CHECK_THAT(cc.getRawContext()->IO.DisplaySize.x, WithinAbs(cc.size().x / 2.0f, 1e-3));

    const ImVec2 outer_center = (inner.button_min + inner.button_max) * 0.5f * cc.scale() + cc.origin();
    ctx.click(outer_center, draw);
    CHECK(inner.clicks == 1);

    // Near the bottom right corner of the zoomed button, which is outside of the unzoomed one
    const ImVec2 corner = cc.origin() + inner.button_max * cc.scale() - ImVec2(2, 2);
    REQUIRE(corner.x > cc.origin().x + inner.button_max.x);
    ctx.click(corner, draw);
    CHECK(inner.clicks == 2);
}

TEST_CASE("ContainedContext zooms with the mouse wheel within its limits", "[utils][imgui_context_wrapper]")
{
    WrapperEnv env;
    HeadlessContext ctx;
    ContainedContext cc;
    cc.config().size = ImVec2(600, 400);
    cc.config().zoom_smoothness = 0.0f; // Immediate
    cc.config().zoom_max = 1.5f;

    Inner inner;
    auto draw = [&]() { drawContained(cc, inner); };
    ctx.frame(draw);
    ctx.mouseMove(cc.origin() + ImVec2(300, 200));
    ctx.frame(draw);
    REQUIRE(cc.hovered());

    ctx.mouseWheel(2.0f); // Two zoom divisions of 0.1
    ctx.frame(draw);
    CHECK_THAT(cc.scale(), WithinAbs(1.2, 1e-5));

    for (int i = 0; i < 10; i++)
    {
        ctx.mouseWheel(1.0f);
        ctx.frame(draw);
    }
    CHECK_THAT(cc.scale(), WithinAbs(1.5, 1e-5));
}

TEST_CASE("ContainedContext does not save an imgui.ini", "[utils][imgui_context_wrapper]")
{
    WrapperEnv env;
    {
        HeadlessContext ctx;
        ContainedContext cc;
        cc.config().extra_window_wrapper = true; // Creates a window, which has settings to save
        cc.config().size = ImVec2(600, 400);

        Inner inner;
        auto draw = [&]() { drawContained(cc, inner); };
        ctx.frame(draw);
        REQUIRE(cc.getRawContext() != nullptr);
        CHECK(cc.getRawContext()->IO.IniFilename == nullptr);

        // Past io.IniSavingRate, ImGui would save the settings of a moved or resized window
        ctx.frames(3, draw);
    } // Destroying a context saves its settings too

    CHECK_FALSE(std::filesystem::exists(env.dir.path / "imgui.ini"));
}
