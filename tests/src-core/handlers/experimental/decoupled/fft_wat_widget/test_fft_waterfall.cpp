#include "core/style.h"
#include "handlers/experimental/decoupled/fft_wat_widget/fft_waterfall.h"
#include "helpers/guards.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_test_helpers.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <vector>

// The FFT part of FFTWaterfallWidget, drawn in a headless ImGui context

using satdump::widgets::FFTWaterfallWidget;

namespace
{
    // Horizontal extent of the vertices of the test window with the given colour
    bool xExtent(ImU32 col, float &min_x, float &max_x)
    {
        ImGuiWindow *w = ImGui::FindWindowByName("##satdump_tests");
        bool found = false;
        min_x = 1e9f;
        max_x = -1e9f;
        for (const ImDrawVert &v : w->DrawList->VtxBuffer)
            if (v.col == col)
            {
                found = true;
                min_x = std::min(min_x, v.pos.x);
                max_x = std::max(max_x, v.pos.x);
            }
        return found;
    }
} // namespace

TEST_CASE("FFTWaterfallWidget shades both sides of a VFO band", "[handlers][fft_waterfall]")
{
    test_helpers::ValueGuard<float> scale_guard(ui_scale, 1.0f);
    imgui_test::HeadlessContext ctx;

    std::vector<float> fft(1024, -100.0f);
    FFTWaterfallWidget widget(1024, 100);
    widget.set_fft_ptr(fft.data());
    widget.set_fft_size((int)fft.size());
    widget.allow_user_interactions = false;
    widget.frequency = 100e6;
    widget.bandwidth = 6e6;
    // 1 MHz wide, centred: from 99.5 to 100.5 MHz, so 50 px either side of the centre at 600 px for 6 MHz
    widget.vfo_freqs = {{"vfo", 100e6, 1e6}};

    ImVec2 pos;
    ctx.frame(
        [&]()
        {
            pos = ImGui::GetCursorScreenPos();
            widget.draw(ImVec2(600, 300), false);
        });

    const float center = pos.x + 300;
    float min_x, max_x;
    REQUIRE(xExtent(IM_COL32(255, 255, 255, 100), min_x, max_x));
    // The lower half (drawn from the centre leftwards) used to be passed as an inverted rectangle,
    // which Dear ImGui 1.93 does not draw
    CHECK(min_x <= center - 49);
    CHECK(min_x >= center - 51);
    CHECK(max_x >= center + 49);
    CHECK(max_x <= center + 51);
}
