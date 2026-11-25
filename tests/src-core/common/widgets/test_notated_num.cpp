#include "common/widgets/notated_num.h"
#include "imgui/imgui_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

using satdump::widgets::NotatedNum;

// draw() is tested in a headless ImGui context

TEST_CASE("NotatedNum formats its initial value", "[common][widgets][notated_num]")
{
    NotatedNum<double> num("##id", 2400000, "sps");
    CHECK(num.get() == 2400000);
    CHECK(num.display_val == "2.4 Msps");

    num.set(137.1e6);
    CHECK(num.get() == 137.1e6);
    CHECK(num.display_val == "137.1 Msps");
}

TEST_CASE("NotatedNum::parse_input understands SI suffixes and units", "[common][widgets][notated_num]")
{
    NotatedNum<uint64_t> num("##freq", 0, "Hz");

    struct Case
    {
        std::string input;
        uint64_t value;
        std::string display;
    };
    for (Case c : {Case{"137.1M", 137100000, "137.1 MHz"}, Case{"137.1 MHz", 137100000, "137.1 MHz"}, Case{"1.7 ghz", 1700000000, "1.7 GHz"}, Case{"250k", 250000, "250 kHz"},
                   Case{"  42  ", 42, "42 Hz"}, Case{"5.5T", 5500000000000ULL, "5.5 THz"}, Case{"2P", 2000000000000000ULL, "2 PHz"}})
    {
        num.display_val = c.input;
        num.parse_input();
        INFO(c.input);
        CHECK(num.get() == c.value);
        CHECK(num.display_val == c.display);
    }
}

TEST_CASE("NotatedNum::parse_input reverts invalid input", "[common][widgets][notated_num]")
{
    NotatedNum<double> num("##id", 1000, "Hz");
    REQUIRE(num.display_val == "1 kHz");

    num.display_val = "garbage";
    num.parse_input();
    CHECK(num.get() == 1000);
    CHECK(num.display_val == "1 kHz");

    // Only units and whitespace: nothing left to parse
    num.display_val = " Hz ";
    num.parse_input();
    CHECK(num.get() == 1000);
    CHECK(num.display_val == "1 kHz");
}

TEST_CASE("NotatedNum<int> does not accept T and P suffixes", "[common][widgets][notated_num]")
{
    NotatedNum<int> num("##int", 0, "");

    num.display_val = "1G";
    num.parse_input();
    CHECK(num.get() == 1000000000);

    num.display_val = "-3k";
    num.parse_input();
    CHECK(num.get() == -3000);

    // 'T' is not a suffix for int, so stod parses the leading "3" and ignores the rest
    num.display_val = "3T";
    num.parse_input();
    CHECK(num.get() == 3);
}

TEST_CASE("NotatedNum<int64_t> handles negative values", "[common][widgets][notated_num]")
{
    NotatedNum<int64_t> num("##off", -5000, "Hz");
    CHECK(num.display_val == "-5 kHz");

    num.display_val = "-1.25M";
    num.parse_input();
    CHECK(num.get() == -1250000);
    CHECK(num.display_val == "-1.25 MHz");
}

TEST_CASE("NotatedNum::draw parses what is typed once the input is left", "[common][widgets][notated_num]")
{
    imgui_test::HeadlessContext ctx;
    NotatedNum<uint64_t> num("##freq", 100000000, "Hz");
    ImVec2 center;
    int edits = 0;
    auto draw = [&]()
    {
        if (num.draw())
            edits++;
        center = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
    };
    ctx.frame(draw);
    CHECK(edits == 0);

    // The whole text is selected on click, so typing replaces it
    ctx.click(center, draw);
    ctx.typeText("1.7G");
    ctx.frame(draw);
    CHECK(num.display_val == "1.7G");
    CHECK(num.get() == 100000000); // Not parsed while editing
    CHECK(edits == 0);

    ctx.pressKey(ImGuiKey_Enter, draw);
    CHECK(edits == 1);
    CHECK(num.get() == 1700000000);
    CHECK(num.display_val == "1.7 GHz");

    // Invalid input is reverted
    ctx.click(center, draw);
    ctx.typeText("abc");
    ctx.pressKey(ImGuiKey_Enter, draw);
    CHECK(edits == 2);
    CHECK(num.get() == 1700000000);
    CHECK(num.display_val == "1.7 GHz");
}
