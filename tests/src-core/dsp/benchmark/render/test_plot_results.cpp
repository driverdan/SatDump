#include "dsp/benchmark/render/plot_results.h"
#include "imgui/imgui.h"
#include "imgui/implot/implot.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <map>

// renderResults() draws an ImPlot bar chart with Dear ImGui and the imgui_sw software renderer,
// without a display. It is the only caller of the legacy (pre 1.92) font atlas API, through
// imgui_sw::bind_imgui_painting().

using namespace satdump::ndsp;

namespace
{
    // renderResults() leaves its ImGui and ImPlot contexts current, and never destroys them (#415)
    image::Image render(const std::vector<DSPBenchResult> &results)
    {
        ImGuiContext *before = ImGui::GetCurrentContext();
        ImPlotContext *plot_before = ImPlot::GetCurrentContext();
        image::Image img = renderResults(results);
        if (ImPlot::GetCurrentContext() != plot_before)
            ImPlot::DestroyContext();
        if (ImGui::GetCurrentContext() != before)
            ImGui::DestroyContext();
        ImGui::SetCurrentContext(before);
        ImPlot::SetCurrentContext(plot_before);
        return img;
    }

    // Number of pixels within tol of a colour
    size_t countColor(image::Image &img, int r, int g, int b, int tol = 3)
    {
        size_t n = 0;
        const size_t size = img.width() * img.height();
        for (size_t i = 0; i < size; i++)
            if (std::abs(img.get(0, i) - r) <= tol && std::abs(img.get(1, i) - g) <= tol && std::abs(img.get(2, i) - b) <= tol)
                n++;
        return n;
    }

    // Number of pixels that differ from the most common colour
    size_t countForeground(image::Image &img)
    {
        const size_t size = img.width() * img.height();
        std::map<uint32_t, size_t> counts;
        for (size_t i = 0; i < size; i++)
            counts[img.get(0, i) << 16 | img.get(1, i) << 8 | img.get(2, i)]++;
        size_t most = 0;
        for (auto &c : counts)
            most = std::max(most, c.second);
        return size - most;
    }
} // namespace

TEST_CASE("ndsp renderResults renders a 1024x1024 RGBA bar chart", "[dsp][benchmark][render]")
{
    image::Image img = render({{"fft_ddc", 100e6}, {"agc", 50e6}, {"costas", 25e6}});
    REQUIRE(img.width() == 1024);
    REQUIRE(img.height() == 1024);
    REQUIRE(img.channels() == 4);
    REQUIRE(img.depth() == 8);

    // Opaque
    CHECK(img.get(3, 0) == 255);
    CHECK(img.get(3, 1024 * 1024 - 1) == 255);

    // The bars use the first colours of the default ImPlot colormap (Deep)
    CHECK(countColor(img, 76, 114, 176) > 1000);
    CHECK(countColor(img, 221, 132, 82) > 500);
    CHECK(countColor(img, 85, 168, 104) > 250);

    // and the tallest bar covers more than the shorter ones
    CHECK(countColor(img, 76, 114, 176) > countColor(img, 221, 132, 82));
}

TEST_CASE("ndsp renderResults draws text and axes", "[dsp][benchmark][render]")
{
    // Without results there are no bars, but the frame, the grid, the ticks and their labels
    image::Image img = render({});
    REQUIRE(img.width() == 1024);
    CHECK(countForeground(img) > 5000);
    CHECK(countColor(img, 76, 114, 176) == 0);
}

TEST_CASE("ndsp renderResults is deterministic", "[dsp][benchmark][render]")
{
    image::Image a = render({{"rrc", 10e6}});
    image::Image b = render({{"rrc", 10e6}});
    REQUIRE(a.width() == b.width());
    size_t diff = 0;
    for (size_t i = 0; i < a.width() * a.height(); i++)
        if (a.get(0, i) != b.get(0, i) || a.get(1, i) != b.get(1, i) || a.get(2, i) != b.get(2, i))
            diff++;
    CHECK(diff == 0); // Deterministic
}
