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

renderer::renderer(std::shared_ptr<GLFWwindow> window)
    : _window(std::move(window))
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

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
    glClearColor(0.055f, 0.059f, 0.071f, 1.0f);
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
