#include "imgui/imgui_image.h"
#include "imgui/imgui_test_helpers.h"
#include <catch2/catch_test_macros.hpp>

// useImageTextureFilters() asks the renderer backend to use the filters of the textures
// (GL_NEAREST, mipmaps) instead of its own linear sampler. Without a GPU, the test checks that
// the request is the first command the backend gets.

using imgui_test::HeadlessContext;

namespace
{
    void setSamplerFromTex(const ImDrawList *, const ImDrawCmd *) {}
} // namespace

TEST_CASE("useImageTextureFilters makes the backend use the texture filters for the whole frame", "[imgui][imgui_image]")
{
    HeadlessContext ctx;
    ImGui::GetPlatformIO().DrawCallback_SetSamplerFromTex = setSamplerFromTex;

    // Called at the start of the frame, before anything is drawn
    ctx.frame(
        []()
        {
            useImageTextureFilters();
            ImGui::Text("Some text");
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)1234, ImVec2(0, 0), ImVec2(100, 100));
        });

    ImDrawData *dd = ImGui::GetDrawData();
    REQUIRE(dd != nullptr);
    REQUIRE(dd->CmdLists.Size >= 2);
    // The first draw list rendered starts with the callback
    const ImDrawList *first = dd->CmdLists[0];
    REQUIRE(first->CmdBuffer.Size >= 1);
    CHECK(first->CmdBuffer[0].UserCallback == setSamplerFromTex);

    // and no other draw list asks for another sampler
    int callbacks = 0;
    for (const ImDrawList *list : dd->CmdLists)
        for (const ImDrawCmd &cmd : list->CmdBuffer)
            if (cmd.UserCallback != nullptr)
                callbacks++;
    CHECK(callbacks == 1);

    // Again on the next frame
    ctx.frame([]() { useImageTextureFilters(); });
    CHECK(ImGui::GetDrawData()->CmdLists[0]->CmdBuffer[0].UserCallback == setSamplerFromTex);
}

TEST_CASE("useImageTextureFilters does nothing if the backend has no such callback", "[imgui][imgui_image]")
{
    HeadlessContext ctx;
    REQUIRE(ImGui::GetPlatformIO().DrawCallback_SetSamplerFromTex == nullptr);
    ctx.frame(
        []()
        {
            useImageTextureFilters();
            ImGui::Text("Some text");
        });
    for (const ImDrawList *list : ImGui::GetDrawData()->CmdLists)
        for (const ImDrawCmd &cmd : list->CmdBuffer)
            CHECK(cmd.UserCallback == nullptr);
}
