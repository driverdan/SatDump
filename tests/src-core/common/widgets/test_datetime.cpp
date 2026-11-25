#include "common/widgets/datetime.h"
#include "core/style.h"
#include "helpers/guards.h"
#include "imgui/imgui_test_helpers.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using satdump::widgets::DateTimePicker;

// draw() is tested in a headless ImGui context

namespace
{
    const double T_2024_01_02_030405 = 1704164645; // 2024-01-02 03:04:05 UTC
} // namespace

TEST_CASE("DateTimePicker round-trips whole second timestamps", "[common][widgets][datetime]")
{
    DateTimePicker picker("##dt", T_2024_01_02_030405);
    CHECK(picker.get() == T_2024_01_02_030405);

    picker.set(0);
    CHECK(picker.get() == 0);

    picker.set(951782400); // 2000-02-29, leap day
    CHECK(picker.get() == 951782400);
}

TEST_CASE("DateTimePicker keeps fractional seconds", "[common][widgets][datetime]")
{
    DateTimePicker picker("##dt", T_2024_01_02_030405 + 0.5);
    CHECK(picker.get() == Catch::Approx(T_2024_01_02_030405 + 0.5).epsilon(0).margin(1e-6));

    picker.set(T_2024_01_02_030405 + 0.25);
    CHECK(picker.get() == Catch::Approx(T_2024_01_02_030405 + 0.25).epsilon(0).margin(1e-6));

    picker.set(T_2024_01_02_030405 + 0.123);
    CHECK(picker.get() == Catch::Approx(T_2024_01_02_030405 + 0.123).epsilon(0).margin(1e-5));
}

TEST_CASE("DateTimePicker uses -1 for automatic time", "[common][widgets][datetime]")
{
    DateTimePicker picker("##dt", -1);
    CHECK(picker.get() == -1.0);

    picker.set(T_2024_01_02_030405);
    CHECK(picker.get() == T_2024_01_02_030405);

    picker.set(-1);
    CHECK(picker.get() == -1.0);
}

// Bug: datetime.cpp:37-48 stores the fraction as the integer digits after the
// decimal point and rescales by its digit count, dropping leading zeros, so
// .05 comes back as .5 (and .001 as .1).
TEST_CASE("DateTimePicker keeps fractional seconds with leading zeros", "[common][widgets][datetime][!shouldfail]")
{
    DateTimePicker picker("##dt", T_2024_01_02_030405 + 0.05);
    CHECK(picker.get() == Catch::Approx(T_2024_01_02_030405 + 0.05).epsilon(0).margin(1e-5));
}

TEST_CASE("DateTimePicker::draw keeps the time while it is drawn", "[common][widgets][datetime]")
{
    test_helpers::ValueGuard<float> scale_guard(ui_scale, 1.0f);
    imgui_test::HeadlessContext ctx;
    DateTimePicker fixed("##fixed", T_2024_01_02_030405);
    DateTimePicker date_only("##date", T_2024_01_02_030405, true);
    DateTimePicker automatic("##auto", -1);
    ctx.frames(3,
               [&]()
               {
                   ImGui::PushID("fixed");
                   fixed.draw();
                   ImGui::PopID();
                   ImGui::PushID("date");
                   date_only.draw();
                   ImGui::PopID();
                   ImGui::PushID("auto");
                   automatic.draw();
                   ImGui::PopID();
               });
    CHECK(fixed.get() == T_2024_01_02_030405);
    CHECK(date_only.get() == T_2024_01_02_030405);
    CHECK(automatic.get() == -1.0);
    CHECK(ctx.lastVertexCount() > 0);
}

TEST_CASE("DateTimePicker::draw edits the year and unticks automatic time", "[common][widgets][datetime]")
{
    test_helpers::ValueGuard<float> scale_guard(ui_scale, 1.0f);
    imgui_test::HeadlessContext ctx;
    DateTimePicker picker("##dt", T_2024_01_02_030405);
    ImVec2 start;
    auto draw = [&]()
    {
        start = ImGui::GetCursorScreenPos();
        picker.draw();
    };
    ctx.frame(draw);

    // The year field follows the "auto" checkbox, after 3 + 3 pixels of spacing
    const float h = ImGui::GetFrameHeight();
    const ImVec2 year(start.x + h + 6 + ImGui::CalcTextSize("00").x, start.y + h / 2);
    ctx.click(year, draw);
    ctx.key(ImGuiMod_Ctrl, true);
    ctx.pressKey(ImGuiKey_A, draw);
    ctx.key(ImGuiMod_Ctrl, false);
    ctx.typeText("2030");
    ctx.frame(draw);
    ctx.pressKey(ImGuiKey_Enter, draw);
    CHECK(picker.get() == 1893553445); // 2030-01-02 03:04:05 UTC

    // Ticking the checkbox switches to automatic time
    ctx.click(ImVec2(start.x + h / 2, start.y + h / 2), draw);
    CHECK(picker.get() == -1.0);
}

TEST_CASE("DateTimePicker::draw moves to the month once four year digits are typed", "[common][widgets][datetime]")
{
    test_helpers::ValueGuard<float> scale_guard(ui_scale, 1.0f);
    imgui_test::HeadlessContext ctx;
    DateTimePicker picker("##dt", T_2024_01_02_030405);
    ImVec2 start;
    auto draw = [&]()
    {
        start = ImGui::GetCursorScreenPos();
        picker.draw();
    };
    ctx.frame(draw);

    const float h = ImGui::GetFrameHeight();
    ctx.click(ImVec2(start.x + h + 6 + ImGui::CalcTextSize("00").x, start.y + h / 2), draw);
    ctx.key(ImGuiMod_Ctrl, true);
    ctx.pressKey(ImGuiKey_A, draw);
    ctx.key(ImGuiMod_Ctrl, false);
    // The fourth digit moves the keyboard focus to the month, whose text is then selected
    ctx.typeText("2030");
    ctx.frames(3, draw);
    ctx.typeText("5");
    ctx.frame(draw);
    ctx.pressKey(ImGuiKey_Enter, draw);
    CHECK(picker.get() == 1903921445); // 2030-05-02 03:04:05 UTC
}
