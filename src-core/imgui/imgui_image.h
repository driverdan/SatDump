#pragma once

#include "imgui.h"
#include <cstdint>
#include <functional>
#include "dll_export.h"

SATDUMP_DLL extern size_t maxTextureSize;
SATDUMP_DLL extern std::function<unsigned int()> makeImageTexture;
SATDUMP_DLL extern std::function<void(unsigned int, uint32_t *, int, int)> updateImageTexture;
SATDUMP_DLL extern std::function<void(unsigned int, uint32_t *, int, int)> updateMMImageTexture;
SATDUMP_DLL extern std::function<void(unsigned int)> deleteImageTexture;

void ushort_to_rgba(uint16_t *input, uint32_t *output, int size, int channels = 1);

void uchar_to_rgba(uint8_t *input, uint32_t *output, int size, int channels = 1);

// Makes the renderer backend sample textures with their own filters (GL_NEAREST, mipmaps...) for
// the whole frame. Since Dear ImGui 1.92.8 the OpenGL 3 backend otherwise binds its own linear
// sampler, which blurs zoomed images. Call once per frame, after ImGui::NewFrame().
SATDUMP_DLL void useImageTextureFilters();