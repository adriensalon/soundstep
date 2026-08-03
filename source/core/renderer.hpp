#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <imgui.h>

#ifdef __ANDROID__
struct ANativeWindow;
#else
struct GLFWwindow;
#endif

namespace soundstep {

struct playback_status;

struct renderer_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct renderer_texture {
    std::uintptr_t native_id { 0 };
    int width { 0 };
    int height { 0 };
};

struct renderer {
#ifdef __ANDROID__
    explicit renderer(ANativeWindow* window);
#else
    explicit renderer(std::shared_ptr<GLFWwindow> window);
#endif
    renderer(const renderer& other) = delete;
    renderer& operator=(const renderer& other) = delete;
    renderer(renderer&& other) = delete;
    renderer& operator=(renderer&& other) = delete;
    ~renderer();

    [[nodiscard]] ImFont* add_font(std::string_view resource_path, float font_size);
    void merge_font(std::string_view resource_path, float font_size, ImFont* destination, const ImWchar* glyph_ranges);
    void begin_frame();
    void render(const playback_status& playback);
    [[nodiscard]] static renderer_texture create_rgba_texture(const unsigned char* pixels, int width, int height);
    static void destroy_texture(renderer_texture texture) noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation { nullptr };
};

}
