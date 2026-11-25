#include "common/rimgui.h"
#include "helpers/guards.h"
#include "imgui/imgui_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>

// RImGui records the widgets a "server" draws, sends them to a "client" which draws them with
// ImGui, and sends back what the user changed. Both sides run here, the client in a headless
// ImGui context.

using imgui_test::HeadlessContext;

namespace
{
    // Makes RImGui record into a server instance instead of drawing locally
    struct RemoteServer
    {
        RImGui::RImGui inst;
        test_helpers::ValueGuard<bool> local_guard{RImGui::is_local, false};
        test_helpers::ValueGuard<RImGui::RImGui *> inst_guard{RImGui::current_instance, &inst};
    };

    std::vector<RImGui::UiElem> encodeDecode(const std::vector<RImGui::UiElem> &elems)
    {
        std::vector<uint8_t> buffer(65536);
        int len = RImGui::encode_vec(buffer.data(), elems);
        return RImGui::decode_vec(buffer.data(), len);
    }

    // Draws the elements on the client, returning the center of the last one
    ImVec2 clientFrame(HeadlessContext &ctx, RImGui::RImGui &client, const std::vector<RImGui::UiElem> &elems, std::vector<RImGui::UiElem> *feedback)
    {
        ImVec2 center;
        ctx.frame(
            [&]()
            {
                auto f = RImGui::draw(&client, elems);
                if (feedback && !f.empty())
                    *feedback = f;
                center = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
            });
        return center;
    }
} // namespace

TEST_CASE("RImGui encodes and decodes UI elements", "[common][rimgui]")
{
    std::vector<RImGui::UiElem> elems = {
        {RImGui::UI_ELEMENT_TEXT, 0, 0, 0, "Some text"},
        {RImGui::UI_ELEMENT_BUTTON, 1, 120.5f, 30, "Start"},
        {RImGui::UI_ELEMENT_COMBO, 2, 0, 0, "Mode", 1, false, 0, 0, 0, std::string("A\0B\0", 4)},
        {RImGui::UI_ELEMENT_INPUTDOUBLE, 3, 0, 0, "Freq", 0, false, 0.5f, 137.1e6, 10, "%.1f"},
        {RImGui::UI_ELEMENT_CHECKBOX, 4, 0, 0, "Enable", 0, true, 0, 0, 0, "", true},
    };
    auto decoded = encodeDecode(elems);
    REQUIRE(decoded.size() == elems.size());
    for (size_t i = 0; i < elems.size(); i++)
    {
        CAPTURE(i);
        CHECK(decoded[i] == elems[i]);
    }
}

TEST_CASE("RImGui draws locally when is_local is set", "[common][rimgui]")
{
    test_helpers::ValueGuard<bool> local_guard(RImGui::is_local, true);
    HeadlessContext ctx;
    int clicks = 0;
    bool checked = false;
    ImVec2 button, checkbox;
    auto draw = [&]()
    {
        RImGui::Text("Value %d", 5);
        if (RImGui::Button("Go"))
            clicks++;
        button = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
        RImGui::Checkbox("Check", &checked);
        checkbox = ImGui::GetItemRectMin() + ImVec2(ImGui::GetFrameHeight() * 0.5f, ImGui::GetFrameHeight() * 0.5f);
    };
    ctx.frame(draw);
    ctx.click(button, draw);
    CHECK(clicks == 1);
    ctx.click(checkbox, draw);
    CHECK(checked);
}

TEST_CASE("RImGui sends a button click from the client back to the server", "[common][rimgui]")
{
    RemoteServer server;
    RImGui::RImGui client;
    HeadlessContext ctx;

    // Server frame: records the widgets
    RImGui::Text("Status");
    CHECK_FALSE(RImGui::Button("Start"));
    auto elems = encodeDecode(RImGui::end_frame());
    REQUIRE(elems.size() == 2);
    CHECK(elems[1].t == RImGui::UI_ELEMENT_BUTTON);

    // Client: draws them and clicks the button
    std::vector<RImGui::UiElem> feedback;
    ImVec2 center = clientFrame(ctx, client, elems, &feedback);
    CHECK(feedback.empty());
    ctx.mouseMove(center);
    clientFrame(ctx, client, {}, &feedback);
    ctx.mouseButton(ImGuiMouseButton_Left, true);
    clientFrame(ctx, client, {}, &feedback);
    ctx.mouseButton(ImGuiMouseButton_Left, false);
    clientFrame(ctx, client, {}, &feedback);
    REQUIRE(feedback.size() == 1);
    CHECK(feedback[0].clicked);

    // Server frame with the feedback
    RImGui::set_feedback(encodeDecode(feedback));
    RImGui::Text("Status");
    CHECK(RImGui::Button("Start"));
    RImGui::end_frame();
}

TEST_CASE("RImGui sends a checkbox change from the client back to the server", "[common][rimgui]")
{
    RemoteServer server;
    RImGui::RImGui client;
    HeadlessContext ctx;

    bool enabled = false;
    RImGui::Checkbox("Enabled", &enabled);
    auto elems = encodeDecode(RImGui::end_frame());
    REQUIRE(elems.size() == 1);

    std::vector<RImGui::UiElem> feedback;
    ImVec2 center = clientFrame(ctx, client, elems, &feedback);
    // Click on the box, not on the label
    center.x = ImGui::GetStyle().WindowPadding.x + ImGui::GetFrameHeight() * 0.5f;
    ctx.mouseMove(center);
    clientFrame(ctx, client, {}, &feedback);
    ctx.mouseButton(ImGuiMouseButton_Left, true);
    clientFrame(ctx, client, {}, &feedback);
    ctx.mouseButton(ImGuiMouseButton_Left, false);
    clientFrame(ctx, client, {}, &feedback);
    REQUIRE(feedback.size() == 1);
    CHECK(feedback[0].bv);

    RImGui::set_feedback(encodeDecode(feedback));
    CHECK(RImGui::Checkbox("Enabled", &enabled));
    CHECK(enabled);
    RImGui::end_frame();
}

TEST_CASE("RImGui end_frame only resends the elements when they changed", "[common][rimgui]")
{
    RemoteServer server;
    RImGui::Text("Same");
    CHECK(RImGui::end_frame().size() == 1);
    RImGui::Text("Same");
    CHECK(RImGui::end_frame().empty());
    RImGui::Text("Different");
    CHECK(RImGui::end_frame().size() == 1);
    RImGui::Text("Different");
    CHECK(RImGui::end_frame(true).size() == 1);
}

TEST_CASE("RImGui client draws every element type", "[common][rimgui]")
{
    RemoteServer server;
    RImGui::RImGui client;
    HeadlessContext ctx;

    int combo = 1, slider_i = 3;
    float slider_f = 0.5f;
    double value = 1.25;
    bool check = true;
    std::string text = "abc";
    RImGui::Text("Text");
    RImGui::Button("Button");
    RImGui::RadioButton("Radio", true);
    RImGui::SameLine();
    RImGui::SteppedSliderInt("SliderI", &slider_i, 0, 10);
    RImGui::SteppedSliderFloat("SliderF", &slider_f, 0, 1);
    RImGui::Checkbox("Check", &check);
    RImGui::Combo("Combo", &combo, "A\0B\0C\0");
    RImGui::InputDouble("Double", &value);
    RImGui::Separator();
    RImGui::beginDisabled();
    RImGui::InputText("Text input", &text);
    RImGui::IsItemDeactivatedAfterEdit();
    RImGui::endDisabled();
    auto elems = encodeDecode(RImGui::end_frame());
    CHECK(elems.size() == 14);

    std::vector<RImGui::UiElem> feedback;
    clientFrame(ctx, client, elems, &feedback);
    clientFrame(ctx, client, {}, &feedback);
    // Nothing was changed by the user
    CHECK(feedback.empty());
    CHECK(ctx.lastVertexCount() > 0);
}
