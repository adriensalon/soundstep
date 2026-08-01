#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <imgui.h>

struct GLFWwindow;

namespace soundstep {

struct renderer_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct renderer_texture {
    std::uintptr_t native_id { 0 };
    int width { 0 };
    int height { 0 };
};

struct renderer {
    explicit renderer(std::shared_ptr<GLFWwindow> window);
    ~renderer();

    renderer(const renderer& other) = delete;
    renderer& operator=(const renderer& other) = delete;
    renderer(renderer&& other) = delete;
    renderer& operator=(renderer&& other) = delete;

    [[nodiscard]] ImFont* add_font(std::string_view resource_path, float font_size);
    void merge_font(std::string_view resource_path, float font_size, const ImWchar* glyph_ranges);
    void begin_frame();
    void render();
    [[nodiscard]] static renderer_texture create_rgba_texture(const unsigned char* pixels, int width, int height);
    static void destroy_texture(renderer_texture texture) noexcept;

private:
    std::shared_ptr<GLFWwindow> _window { nullptr };
};

}
