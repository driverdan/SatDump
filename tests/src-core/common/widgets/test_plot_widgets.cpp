#include "common/widgets/constellation.h"
#include "common/widgets/histogram.h"
#include "common/widgets/snr_plot.h"
#include "common/widgets/themed_widgets.h"
#include "common/widgets/value_plot.h"
#include "core/style.h"
#include "helpers/guards.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_test_helpers.h"
#include "imgui/implot/implot_internal.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

// The constellation, histogram, SNR and value plot widgets, drawn in a headless ImGui context

using imgui_test::HeadlessContext;
using imgui_test::HeadlessOptions;

namespace
{
    // Vertices of the test window's draw list with the given colour
    std::vector<ImVec2> verticesWithColor(ImU32 col)
    {
        std::vector<ImVec2> r;
        ImGuiWindow *w = ImGui::FindWindowByName("##satdump_tests");
        if (!w)
            return r;
        for (const ImDrawVert &v : w->DrawList->VtxBuffer)
            if (v.col == col)
                r.push_back(v.pos);
        return r;
    }

    struct WidgetEnv
    {
        test_helpers::ValueGuard<float> ui_scale_guard{ui_scale, 1.0f};
        test_helpers::ValueGuard<style::Theme> theme_guard{style::theme};

        WidgetEnv()
        {
            style::theme.constellation = ImColor(10, 200, 30, 255);
            style::theme.widget_bg = ImColor(5, 5, 5, 255);
            style::theme.plot_bg = ImColor(20, 20, 20, 255);
            style::theme.red = ImColor(250, 1, 2, 255);
            style::theme.green = ImColor(3, 250, 4, 255);
            style::theme.orange = ImColor(250, 150, 5, 255);
        }
    };
} // namespace

TEST_CASE("ConstellationViewer draws one dot per sample at its IQ position", "[common][widgets][constellation]")
{
    WidgetEnv env;
    HeadlessContext ctx;
    satdump::widgets::ConstellationViewer viewer(1, 1, 200);

    std::vector<complex_t> samples(CONST_SIZE, complex_t(0.5f, -0.5f));
    viewer.pushComplex(samples.data(), (int)samples.size());

    ImVec2 origin, size;
    ctx.frames(2,
               [&]()
               {
                   origin = ImGui::GetCursorScreenPos();
                   viewer.draw();
                   size = ImGui::GetItemRectSize();
               });
    CHECK(size.x == 203.0f);
    CHECK(size.y == 203.0f);

    // (0.5, -0.5) is a quarter of the size right of and above the centre
    const ImVec2 expected(origin.x + 100 + 50, origin.y + 100 - 50);
    auto dots = verticesWithColor(style::theme.constellation);
    REQUIRE(dots.size() >= CONST_SIZE);
    float worst = 0;
    for (ImVec2 p : dots)
        worst = std::max(worst, std::hypot(p.x - expected.x, p.y - expected.y));
    CHECK(worst <= 3.0f); // The dots have a radius of 2
}

TEST_CASE("ConstellationViewer scales the dots with hscale and vscale", "[common][widgets][constellation]")
{
    WidgetEnv env;
    HeadlessContext ctx;
    satdump::widgets::ConstellationViewer viewer(2, 0.5f, 200);
    std::vector<complex_t> samples(CONST_SIZE, complex_t(0.25f, 0.5f));
    viewer.pushComplexScaled(samples.data(), (int)samples.size(), 2.0f);

    ImVec2 origin;
    ctx.frame(
        [&]()
        {
            origin = ImGui::GetCursorScreenPos();
            viewer.draw();
        });
    // (0.5, 1.0) after the push scale, times (2, 0.5)
    const ImVec2 expected(origin.x + 100 + 100, origin.y + 100 + 50);
    auto dots = verticesWithColor(style::theme.constellation);
    REQUIRE(dots.size() >= CONST_SIZE);
    float worst = 0;
    for (ImVec2 p : dots)
        worst = std::max(worst, std::hypot(p.x - expected.x, p.y - expected.y));
    CHECK(worst <= 3.0f);
}

TEST_CASE("HistoViewer draws an ImPlot histogram", "[common][widgets][histogram]")
{
    WidgetEnv env;
    HeadlessOptions opts;
    opts.implot = true;
    HeadlessContext ctx(opts);
    satdump::widgets::HistoViewer viewer(1, 1, 200);

    std::vector<float> soft(HIST_SIZE);
    for (int i = 0; i < HIST_SIZE; i++)
        soft[i] = (i % 2) ? 1.0f : -1.0f;
    viewer.pushFloatAndGaussian(soft.data(), (int)soft.size());

    ImVec2 size;
    bool plot_open = true;
    ctx.frames(3,
               [&]()
               {
                   viewer.draw();
                   size = ImGui::GetItemRectSize();
                   plot_open = ImPlot::GetCurrentContext()->CurrentPlot != nullptr;
               });
    CHECK(size.x == 200.0f);
    CHECK(size.y == 200.0f);
    CHECK_FALSE(plot_open); // EndPlot() was called
    CHECK(ctx.lastVertexCount() > 200);

    // The "Real" bars are filled at half alpha (ImPlotSpec::FillAlpha), the "Imag" bars are not
    ImGuiWindow *w = ImGui::FindWindowByName("##satdump_tests");
    REQUIRE(w != nullptr);
    const ImU32 rgb_mask = ~IM_COL32_A_MASK;
    const ImU32 real_rgb = ImPlot::GetColormapColorU32(0, IMPLOT_AUTO) & rgb_mask;
    const ImU32 imag_rgb = ImPlot::GetColormapColorU32(1, IMPLOT_AUTO) & rgb_mask;
    REQUIRE(real_rgb != imag_rgb);
    auto isHalfAlpha = [](ImU32 col)
    {
        int a = (col >> IM_COL32_A_SHIFT) & 0xFF;
        return a >= 120 && a <= 135;
    };
    bool real_half = false, imag_half = false, imag_opaque = false;
    for (const ImDrawVert &v : w->DrawList->VtxBuffer)
    {
        if ((v.col & rgb_mask) == real_rgb && isHalfAlpha(v.col))
            real_half = true;
        if ((v.col & rgb_mask) == imag_rgb && isHalfAlpha(v.col))
            imag_half = true;
        if (v.col == (imag_rgb | IM_COL32_A_MASK))
            imag_opaque = true;
    }
    CHECK(real_half);
    CHECK_FALSE(imag_half);
    CHECK(imag_opaque);
}

TEST_CASE("SNRPlotViewer and ValuePlotViewer draw their history", "[common][widgets][snr_plot]")
{
    WidgetEnv env;
    HeadlessContext ctx;
    satdump::widgets::SNRPlotViewer snr;
    satdump::widgets::ValuePlotViewer value;

    ImVec2 snr_size, value_size;
    float level = 0;
    ctx.frames(5,
               [&]()
               {
                   snr.draw(level, 20);
                   snr_size = ImGui::GetItemRectSize();
                   value.draw(level, 10, -10, "Value");
                   value_size = ImGui::GetItemRectSize();
                   level += 2.5f;
               });
    // The PlotLines() frames are 200x50
    CHECK(snr_size.x == 200.0f);
    CHECK(snr_size.y == 50.0f);
    CHECK(value_size.x == 200.0f);
    CHECK(value_size.y == 50.0f);

    // Last frame: SNR 10 (orange), peak 20 (green), average 25 / 200 (red), value 10 (orange)
    CHECK_FALSE(verticesWithColor(style::theme.orange).empty());
    CHECK_FALSE(verticesWithColor(style::theme.green).empty());
    CHECK_FALSE(verticesWithColor(style::theme.red).empty());
}

TEST_CASE("SNRPlotViewer and ValuePlotViewer start with an empty history", "[common][widgets][snr_plot]")
{
    WidgetEnv env;
    HeadlessContext ctx;

    // Default-initialized over storage holding 5.0f, as the demod modules hold them as members
    // of classes with user-provided constructors (#414)
    alignas(satdump::widgets::SNRPlotViewer) unsigned char snr_buf[sizeof(satdump::widgets::SNRPlotViewer)];
    alignas(satdump::widgets::ValuePlotViewer) unsigned char value_buf[sizeof(satdump::widgets::ValuePlotViewer)];
    const float garbage = 5.0f;
    for (size_t i = 0; i + sizeof(float) <= sizeof(snr_buf); i += sizeof(float))
        std::memcpy(&snr_buf[i], &garbage, sizeof(float));
    for (size_t i = 0; i + sizeof(float) <= sizeof(value_buf); i += sizeof(float))
        std::memcpy(&value_buf[i], &garbage, sizeof(float));
    auto *snr = new (snr_buf) satdump::widgets::SNRPlotViewer;
    auto *value = new (value_buf) satdump::widgets::ValuePlotViewer;

    SECTION("SNR")
    {
        ctx.frame([&]() { snr->draw(0, 20); });
        // SNR 0 (red), peak 20 (green), average 0 (red). The old history would average 4.975 (orange)
        CHECK_FALSE(verticesWithColor(style::theme.red).empty());
        CHECK_FALSE(verticesWithColor(style::theme.green).empty());
        CHECK(verticesWithColor(style::theme.orange).empty());
    }

    SECTION("Value")
    {
        ctx.frame([&]() { value->draw(0, 10, 0, "Value"); });
        // Every point of the history is 0, so the line is flat at the bottom of the 50px high plot
        std::vector<ImVec2> line = verticesWithColor(ImGui::GetColorU32(ImGuiCol_PlotLines));
        REQUIRE_FALSE(line.empty());
        float min_y = line[0].y, max_y = line[0].y;
        for (const ImVec2 &p : line)
        {
            min_y = std::min(min_y, p.y);
            max_y = std::max(max_y, p.y);
        }
        CHECK(max_y - min_y < 5.0f); // Line thickness and anti-aliasing, the old history stepped by about 24px
    }

    snr->~SNRPlotViewer();
    value->~ValuePlotViewer();
}

TEST_CASE("ThemedPlotLines uses the given background colour", "[common][widgets][themed_widgets]")
{
    HeadlessContext ctx;
    const ImVec4 bg(0.2f, 0.4f, 0.6f, 1.0f);
    float values[4] = {0, 1, 0, 1};
    ImVec4 frame_bg_after;
    ctx.frame(
        [&]()
        {
            satdump::widgets::ThemedPlotLines(bg, "##lines", values, 4, 0, "", 0, 1, ImVec2(100, 40));
            frame_bg_after = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        });
    // The frame is filled with the background colour, which is popped afterwards
    CHECK(verticesWithColor(ImGui::GetColorU32(bg)).size() >= 4);
    const ImVec4 style_bg = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
    CHECK(frame_bg_after.x == style_bg.x);
    CHECK(frame_bg_after.y == style_bg.y);
    CHECK(frame_bg_after.z == style_bg.z);
    CHECK(frame_bg_after.w == style_bg.w);
}
