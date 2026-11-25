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
#include <catch2/catch_test_macros.hpp>
#include <cmath>
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
}

TEST_CASE("SNRPlotViewer and ValuePlotViewer draw their history", "[common][widgets][snr_plot]")
{
    WidgetEnv env;
    HeadlessContext ctx;
    // Value-initialized, as the widgets do not initialize their history
    satdump::widgets::SNRPlotViewer snr{};
    satdump::widgets::ValuePlotViewer value{};

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
    // The plot of PlotLines() is 200x50, plus the frame padding
    CHECK(snr_size.x == 200.0f);
    CHECK(snr_size.y == 50.0f);
    CHECK(value_size.x == 200.0f);
    CHECK(value_size.y == 50.0f);
    CHECK(ctx.lastVertexCount() > 0);
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
    CHECK(frame_bg_after.x == ImGui::GetStyle().Colors[ImGuiCol_FrameBg].x);
}
