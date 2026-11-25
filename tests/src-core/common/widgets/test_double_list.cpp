#include "common/widgets/double_list.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_test_helpers.h"
#include <catch2/catch_test_macros.hpp>

using satdump::widgets::DoubleList;

// render() is tested in a headless ImGui context

TEST_CASE("DoubleList selects from a fixed list", "[common][widgets][double_list]")
{
    DoubleList list("Samplerate");
    list.set_list({250e3, 1e6, 2.4e6}, false);

    CHECK(list.get_list().size() == 3);
    CHECK(list.get_value() == 250e3); // First entry by default

    CHECK(list.set_value(2.4e6));
    CHECK(list.get_value() == 2.4e6);

    // Values outside the list are rejected without manual entry
    CHECK_FALSE(list.set_value(3e6));
    CHECK(list.get_value() == 2.4e6);
}

TEST_CASE("DoubleList with manual entry accepts any value", "[common][widgets][double_list]")
{
    DoubleList list("Samplerate");
    list.set_list({1e6, 2e6}, true);

    // A "Manual" placeholder entry is appended
    REQUIRE(list.get_list().size() == 3);
    CHECK(list.get_list().back() == -1);
    CHECK(list.get_value() == 1e6);

    CHECK(list.set_value(3.2e6));
    CHECK(list.get_value() == 3.2e6);

    // While in manual mode a listed value keeps manual mode but updates the value
    CHECK(list.set_value(2e6));
    CHECK(list.get_value() == 2e6);
}

TEST_CASE("DoubleList set_list resets the selection", "[common][widgets][double_list]")
{
    DoubleList list("Rate");
    list.set_list({1, 2, 3}, false);
    list.set_value(3);
    CHECK(list.get_value() == 3);

    list.set_list({10, 20}, false);
    CHECK(list.get_value() == 10);
}

TEST_CASE("DoubleList::render selects a listed value in its combo", "[common][widgets][double_list]")
{
    imgui_test::HeadlessContext ctx;
    DoubleList list("Rate");
    list.set_list({1e6, 2e6, 3e6}, false);
    ImVec2 combo;
    bool changed = false;
    auto draw = [&]()
    {
        changed = list.render() || changed;
        combo = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
    };
    ctx.frame(draw);

    // Open the combo, then click on the third item of its popup
    ctx.click(combo, draw);
    ctx.frames(2, draw); // The popup is sized to fit its items
    ImGuiWindow *popup = ImGui::FindWindowByName("##Combo_00");
    REQUIRE(popup != nullptr);
    REQUIRE(popup->Active);
    const float line = ImGui::GetTextLineHeightWithSpacing();
    ImVec2 third(popup->Pos.x + popup->Size.x * 0.5f, popup->Pos.y + ImGui::GetStyle().WindowPadding.y + line * 2.5f);
    ctx.click(third, draw);
    CHECK(changed);
    CHECK(list.get_value() == 3e6);
}

TEST_CASE("DoubleList::render without a list draws the manual input", "[common][widgets][double_list]")
{
    imgui_test::HeadlessContext ctx;
    DoubleList list("Rate");
    ImVec2 input;
    bool changed = false;
    auto draw = [&]()
    {
        changed = list.render() || changed;
        input = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
    };
    ctx.frame(draw);
    ctx.click(input, draw);
    ctx.typeText("6M");
    ctx.pressKey(ImGuiKey_Enter, draw);
    CHECK(changed);
    CHECK(list.get_value() == 6e6);
}
