#pragma once

/*
A Dear ImGui context that runs without a display, for testing the SatDump code that calls
ImGui, ImPlot, ImPlot3D and imnodes.

- HeadlessContext creates an ImGui context (and optionally the ImPlot, ImPlot3D and imnodes
  ones) and makes them current. frame() makes them current again, in case other code
  changed the current contexts in between. On destruction it destroys them and restores the
  contexts that were current before. Settings and log files are disabled for its context,
  so it writes nothing to the disk.
- It acts as a renderer backend that supports ImGuiBackendFlags_RendererHasTextures, like
  the SatDump OpenGL backends do: the texture requests ImGui makes are accepted without
  uploading anything, so the font atlas is built on demand.
- frame() runs one ImGui frame and calls the given function inside a window that covers
  the whole display, so widgets can be drawn without a Begin() / End() of their own.
- Input is queued with mouseMove(), mouseButton(), key() and typeText() into the IO of this
  context (whichever context is current), and is processed by the next frame. ImGui handles at most one change of a mouse button or key per frame,
  so click() and pressKey() run the frames that they need.
*/

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imnodes/imnodes.h"
#include "imgui/implot/implot.h"
#include "imgui/implot3d/implot3d.h"
#include <cstdint>
#include <functional>
#include <string>

namespace imgui_test
{
    struct HeadlessOptions
    {
        ImVec2 display_size = ImVec2(1280, 720);
        bool implot = false;
        bool implot3d = false;
        bool imnodes = false;
    };

    class HeadlessContext
    {
    public:
        explicit HeadlessContext(HeadlessOptions options = HeadlessOptions()) : options(options)
        {
            previous = ImGui::GetCurrentContext();
            previous_plot = ImPlot::GetCurrentContext();
            previous_plot3d = ImPlot3D::GetCurrentContext();
            previous_nodes = ImNodes::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);

            ImGuiIO &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.DisplaySize = options.display_size;
            io.DeltaTime = 1.0f / 60.0f;
            io.BackendPlatformName = "satdump_tests";
            io.BackendRendererName = "satdump_tests";
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;

            // The Create functions only make the new context current if none is
            if (options.implot)
                plot_ctx = ImPlot::CreateContext();
            if (options.implot3d)
                plot3d_ctx = ImPlot3D::CreateContext();
            if (options.imnodes)
                nodes_ctx = ImNodes::CreateContext();
            makeCurrent();
        }

        ~HeadlessContext()
        {
            makeCurrent();
            if (nodes_ctx)
                ImNodes::DestroyContext(nodes_ctx);
            if (plot3d_ctx)
                ImPlot3D::DestroyContext(plot3d_ctx);
            if (plot_ctx)
                ImPlot::DestroyContext(plot_ctx);

            // What a renderer backend does on shutdown
            for (ImTextureData *tex : ImGui::GetPlatformIO().Textures)
                if (tex->RefCount == 1)
                    destroyTexture(tex);
            ImGuiIO &io = ImGui::GetIO();
            io.BackendPlatformName = nullptr;
            io.BackendRendererName = nullptr;
            io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);

            ImGui::DestroyContext(ctx);
            ImGui::SetCurrentContext(previous);
            ImPlot::SetCurrentContext(previous_plot);
            ImPlot3D::SetCurrentContext(previous_plot3d);
            ImNodes::SetCurrentContext(previous_nodes);
        }

        HeadlessContext(const HeadlessContext &) = delete;
        HeadlessContext &operator=(const HeadlessContext &) = delete;

        ImGuiContext *context() { return ctx; }
        int frameCount() const { return frame_count; }

        // Runs one frame, calling draw() inside a window covering the display
        void frame(const std::function<void()> &draw = nullptr)
        {
            makeCurrent();
            ImGui::GetIO().DisplaySize = options.display_size;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(options.display_size);
            ImGui::Begin("##satdump_tests", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
            if (draw)
                draw();
            ImGui::End();
            ImGui::Render();
            updateTextures();
            frame_count++;
        }

        void frames(int count, const std::function<void()> &draw = nullptr)
        {
            for (int i = 0; i < count; i++)
                frame(draw);
        }

        // Queued input, processed by the next frame
        void mouseMove(ImVec2 pos) { ctx->IO.AddMousePosEvent(pos.x, pos.y); }
        void mouseButton(ImGuiMouseButton button, bool down) { ctx->IO.AddMouseButtonEvent(button, down); }
        void mouseWheel(float wheel_y) { ctx->IO.AddMouseWheelEvent(0.0f, wheel_y); }
        void key(ImGuiKey key, bool down) { ctx->IO.AddKeyEvent(key, down); }
        void typeText(const std::string &text) { ctx->IO.AddInputCharactersUTF8(text.c_str()); }

        // Moves the mouse to pos and clicks there, over three frames
        void click(ImVec2 pos, const std::function<void()> &draw, ImGuiMouseButton button = ImGuiMouseButton_Left)
        {
            mouseMove(pos);
            frame(draw);
            mouseButton(button, true);
            frame(draw);
            mouseButton(button, false);
            frame(draw);
        }

        // Presses and releases a key, over two frames
        void pressKey(ImGuiKey k, const std::function<void()> &draw)
        {
            key(k, true);
            frame(draw);
            key(k, false);
            frame(draw);
        }

        // Number of vertices drawn by the last frame
        int lastVertexCount()
        {
            makeCurrent();
            ImDrawData *dd = ImGui::GetDrawData();
            return dd && dd->Valid ? dd->TotalVtxCount : 0;
        }

    private:
        void makeCurrent()
        {
            ImGui::SetCurrentContext(ctx);
            if (plot_ctx)
                ImPlot::SetCurrentContext(plot_ctx);
            if (plot3d_ctx)
                ImPlot3D::SetCurrentContext(plot3d_ctx);
            if (nodes_ctx)
                ImNodes::SetCurrentContext(nodes_ctx);
        }

        // What a renderer backend does with the texture requests of ImGui, without a GPU
        void updateTextures()
        {
            for (ImTextureData *tex : ImGui::GetPlatformIO().Textures)
            {
                if (tex->Status == ImTextureStatus_WantCreate)
                {
                    tex->SetTexID((ImTextureID)next_tex_id++); // Unique over the whole context
                    tex->SetStatus(ImTextureStatus_OK);
                }
                else if (tex->Status == ImTextureStatus_WantUpdates)
                    tex->SetStatus(ImTextureStatus_OK);
                else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
                    destroyTexture(tex);
            }
        }

        static void destroyTexture(ImTextureData *tex)
        {
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }

        HeadlessOptions options;
        ImGuiContext *previous = nullptr;
        ImPlotContext *previous_plot = nullptr;
        ImPlot3DContext *previous_plot3d = nullptr;
        ImNodesContext *previous_nodes = nullptr;
        ImGuiContext *ctx = nullptr;
        ImPlotContext *plot_ctx = nullptr;
        ImPlot3DContext *plot3d_ctx = nullptr;
        ImNodesContext *nodes_ctx = nullptr;
        int frame_count = 0;
        intptr_t next_tex_id = 1;
    };
} // namespace imgui_test
