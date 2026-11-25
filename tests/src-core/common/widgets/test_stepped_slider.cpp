#include "common/widgets/stepped_slider.h"
#include "imgui/imgui_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <functional>

// The stepped sliders, drawn in a headless ImGui context: a slider, then "-" and "+" buttons

using Catch::Matchers::WithinAbs;
using imgui_test::HeadlessContext;

namespace
{
    // Draws a stepped slider, and computes where its buttons are
    struct SliderLayout
    {
        ImVec2 minus, plus, slider;
        float group_width = 0;
        bool changed = false;

        void draw(const std::function<bool()> &slider_fn)
        {
            const ImGuiStyle &style = ImGui::GetStyle();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetFrameHeight();
            const float slider_width = ImGui::CalcItemWidth() - (h + style.ItemInnerSpacing.x) * 2;
            changed = slider_fn() || changed;
            group_width = ImGui::GetItemRectSize().x;
            slider = ImVec2(start.x + slider_width / 2, start.y + h / 2);
            minus = ImVec2(start.x + slider_width + style.ItemInnerSpacing.x + h / 2, start.y + h / 2);
            plus = ImVec2(minus.x + h + style.ItemInnerSpacing.x, minus.y);
        }
    };
} // namespace

TEST_CASE("SteppedSliderInt steps with its buttons and clamps to the range", "[common][widgets][stepped_slider]")
{
    HeadlessContext ctx;
    int v = 5;
    SliderLayout l;
    auto draw = [&]() { l.draw([&]() { return satdump::widgets::SteppedSliderInt("Gain", &v, 0, 10, 3); }); };
    ctx.frame(draw);
    CHECK(v == 5);
    CHECK_FALSE(l.changed);
    CHECK(l.group_width > ImGui::CalcTextSize("Gain").x);

    ctx.click(l.plus, draw);
    CHECK(v == 8);
    CHECK(l.changed);

    ctx.click(l.plus, draw);
    CHECK(v == 10); // Clamped to the maximum

    l.changed = false;
    ctx.click(l.minus, draw);
    CHECK(v == 7);
    CHECK(l.changed);
    ctx.click(l.minus, draw);
    ctx.click(l.minus, draw);
    ctx.click(l.minus, draw);
    CHECK(v == 0); // Clamped to the minimum
}

TEST_CASE("SteppedSliderFloat steps with its buttons, finer with Shift and Ctrl", "[common][widgets][stepped_slider]")
{
    HeadlessContext ctx;
    float v = 1.0f;
    SliderLayout l;
    auto draw = [&]() { l.draw([&]() { return satdump::widgets::SteppedSliderFloat("Level", &v, -5.0f, 5.0f, 1.0f); }); };
    ctx.frame(draw);

    ctx.click(l.plus, draw);
    CHECK_THAT(v, WithinAbs(2.0, 1e-6));
    ctx.click(l.minus, draw);
    ctx.click(l.minus, draw);
    CHECK_THAT(v, WithinAbs(0.0, 1e-6));

    ctx.key(ImGuiMod_Shift, true);
    ctx.click(l.plus, draw);
    ctx.key(ImGuiMod_Shift, false);
    CHECK_THAT(v, WithinAbs(0.1, 1e-6));

    ctx.key(ImGuiMod_Ctrl, true);
    ctx.click(l.minus, draw);
    ctx.key(ImGuiMod_Ctrl, false);
    CHECK_THAT(v, WithinAbs(0.09, 1e-6));
}

TEST_CASE("SteppedSliderInt can be dragged like a slider", "[common][widgets][stepped_slider]")
{
    HeadlessContext ctx;
    int v = 0;
    SliderLayout l;
    auto draw = [&]() { l.draw([&]() { return satdump::widgets::SteppedSliderInt("Drag", &v, 0, 100); }); };
    ctx.frame(draw);

    // A click on the middle of the slider sets it to about half the range
    ctx.click(l.slider, draw);
    CHECK(v >= 45);
    CHECK(v <= 55);
    CHECK(l.changed);
}
