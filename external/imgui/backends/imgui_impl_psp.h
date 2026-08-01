// dear imgui: Platform Backend for PSP
// This implements both the platform and the renderer PSP GU

// Implemented features:
//  [X] Platform: Mouse support from the pad.
//  [X] Platform: Gamepad support. Enabled with 'io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad'.

#pragma once
#include "imgui.h"

struct ImGui_ImplPSP_Texture {
    void* pixels = nullptr;
    int psm = 0;
    int width = 0;
    int height = 0;
    int buffer_width = 0;
    int buffer_height = 0;
    int tbw = 0;
};

IMGUI_IMPL_API bool ImGui_ImplPSP_Init();
IMGUI_IMPL_API void ImGui_ImplPSP_Shutdown();
IMGUI_IMPL_API void ImGui_ImplPSP_NewFrame();
IMGUI_IMPL_API bool ImGui_ImplPSP_UpdateFontsTexture(ImFontAtlas* atlas);
IMGUI_IMPL_API void ImGui_ImplPSP_RenderDrawData(ImDrawData* draw_data);
