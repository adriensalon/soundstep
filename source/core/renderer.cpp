#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <cmrc/cmrc.hpp>
#include <imgui.h>

#include <core/renderer.hpp>

CMRC_DECLARE(soundstep_resource);

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace soundstep {
namespace {

    void _apply_dark_grey_palette()
    {
        ImVec4* _colors = ImGui::GetStyle().Colors;
        _colors[ImGuiCol_Border] = ImVec4(0.30f, 0.30f, 0.30f, 0.65f);
        _colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.16f, 0.75f);
        _colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
        _colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
        _colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
        _colors[ImGuiCol_CheckMark] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
        _colors[ImGuiCol_SliderGrab] = ImVec4(0.42f, 0.42f, 0.42f, 1.00f);
        _colors[ImGuiCol_SliderGrabActive] = ImVec4(0.58f, 0.58f, 0.58f, 1.00f);
        _colors[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
        _colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
        _colors[ImGuiCol_ButtonActive] = ImVec4(0.34f, 0.34f, 0.34f, 1.00f);
        _colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.20f, 0.75f);
        _colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
        _colors[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
        _colors[ImGuiCol_SeparatorHovered] = ImVec4(0.38f, 0.38f, 0.38f, 0.78f);
        _colors[ImGuiCol_SeparatorActive] = ImVec4(0.48f, 0.48f, 0.48f, 1.00f);
        _colors[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.30f, 0.30f, 0.20f);
        _colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.42f, 0.42f, 0.42f, 0.67f);
        _colors[ImGuiCol_ResizeGripActive] = ImVec4(0.55f, 0.55f, 0.55f, 0.95f);
        _colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
        _colors[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
        _colors[ImGuiCol_TabActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
        _colors[ImGuiCol_TabUnfocused] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
        _colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
        _colors[ImGuiCol_TableHeaderBg] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
        _colors[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
        _colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
        _colors[ImGuiCol_TextSelectedBg] = ImVec4(0.45f, 0.45f, 0.45f, 0.35f);
        _colors[ImGuiCol_NavHighlight] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    }

}

renderer::renderer(std::shared_ptr<GLFWwindow> window)
    : _window(std::move(window))
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    _apply_dark_grey_palette();
    ImGuiStyle& _style = ImGui::GetStyle();
    _style.FramePadding = ImVec2(5.0f, 5.0f);
    constexpr float _rounding = 6.0f;
    _style.WindowRounding = _rounding;
    _style.ChildRounding = _rounding;
    _style.PopupRounding = _rounding;
    _style.FrameRounding = _rounding;
    _style.ScrollbarRounding = _rounding;
    _style.GrabRounding = _rounding;
    _style.TabRounding = _rounding;

    if (!ImGui_ImplGlfw_InitForOpenGL(_window.get(), true)) {
        ImGui::DestroyContext();
        throw renderer_error("Failed to initialize the ImGui GLFW backend");
    }

    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        throw renderer_error("Failed to initialize the ImGui OpenGL backend");
    }
}

renderer::~renderer()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

ImFont* renderer::add_font(std::string_view resource_path, float font_size)
{
    const std::string _path(resource_path);
    const cmrc::embedded_filesystem _resources = cmrc::soundstep_resource::get_filesystem();
    if (!_resources.is_file(_path)) {
        throw renderer_error("Embedded font not found: " + _path);
    }

    const cmrc::file _data = _resources.open(_path);
    const std::size_t _data_size = static_cast<std::size_t>(_data.end() - _data.begin());
    if (_data_size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw renderer_error("Embedded font is too large: " + _path);
    }

    ImGuiIO& _io = ImGui::GetIO();
    ImFontConfig _config;
    _config.FontDataOwnedByAtlas = false;
    ImFont* _font = _io.Fonts->AddFontFromMemoryTTF(
        const_cast<char*>(_data.begin()),
        static_cast<int>(_data_size),
        font_size,
        &_config);
    if (_font == nullptr) {
        throw renderer_error("Failed to load embedded font: " + _path);
    }
    return _font;
}

void renderer::merge_font(std::string_view resource_path, float font_size, const ImWchar* glyph_ranges)
{
    const std::string _path(resource_path);
    const cmrc::embedded_filesystem _resources = cmrc::soundstep_resource::get_filesystem();
    if (!_resources.is_file(_path)) {
        throw renderer_error("Embedded font not found: " + _path);
    }

    const cmrc::file _data = _resources.open(_path);
    const std::size_t _data_size = static_cast<std::size_t>(_data.end() - _data.begin());
    if (_data_size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw renderer_error("Embedded font is too large: " + _path);
    }

    ImFontConfig _config;
    _config.FontDataOwnedByAtlas = false;
    _config.MergeMode = true;
    _config.PixelSnapH = true;
    _config.GlyphOffset.y = 3.0f;
    ImFont* _font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<char*>(_data.begin()),
        static_cast<int>(_data_size),
        font_size,
        &_config,
        glyph_ranges);
    if (_font == nullptr) {
        throw renderer_error("Failed to merge embedded font: " + _path);
    }
}

void renderer::begin_frame()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void renderer::render()
{
    ImGui::Render();

    int _width = 0;
    int _height = 0;
    glfwGetFramebufferSize(_window.get(), &_width, &_height);
    glViewport(0, 0, _width, _height);
    glClearColor(0.055f, 0.055f, 0.055f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

renderer_texture renderer::create_rgba_texture(
    const unsigned char* pixels,
    int width,
    int height)
{
    if (pixels == nullptr || width <= 0 || height <= 0) {
        throw renderer_error("Invalid texture image");
    }

    GLuint _texture = 0;
    glGenTextures(1, &_texture);
    if (_texture == 0) {
        throw renderer_error("Failed to create texture");
    }

    glBindTexture(GL_TEXTURE_2D, _texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        width,
        height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    return {
        static_cast<std::uintptr_t>(_texture),
        width,
        height
    };
}

void renderer::destroy_texture(renderer_texture texture) noexcept
{
    const GLuint _texture = static_cast<GLuint>(texture.native_id);
    if (_texture != 0) {
        glDeleteTextures(1, &_texture);
    }
}

}
