#include <stdexcept>

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <imgui.h>

#include <core/context.hpp>
#include <core/integration.hpp>
#include <core/window.hpp>
#include <widget/app.hpp>
#include <widget/settings.hpp>

namespace soundstep {

window::window(context& ctx)
    : _ctx(ctx)
{
    if (glfwInit() != GLFW_TRUE) {
        throw window_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* _native_window = glfwCreateWindow(1280, 720, "SoundStep", nullptr, nullptr);
    if (_native_window == nullptr) {
        glfwTerminate();
        throw window_error("Failed to create the SoundStep window");
    }

    _window = std::shared_ptr<GLFWwindow>(_native_window, glfwDestroyWindow);
    glfwMakeContextCurrent(_native_window);
    glfwSwapInterval(1);

    try {
        _renderer = std::make_unique<renderer>(_window);
#ifdef _WIN32
        _media_transport = std::make_unique<system_media_transport>(glfwGetWin32Window(_native_window));
#endif
        _ctx.fonts.ui = _renderer->add_font("font/pp fraktion.otf", 16.0f);
        static constexpr ImWchar _icon_ranges[] = {
            0xf13d, 0xf13d,
            0xf150, 0xf150,
            0xf190, 0xf190,
            0xf32b, 0xf32b,
            0xf34c, 0xf34c,
            0xf369, 0xf369,
            0xf36d, 0xf36d,
            0xf4e4, 0xf4e4,
            0xf569, 0xf569,
            0xf5a1, 0xf5a1,
            0xf5a8, 0xf5a8,
            0xf605, 0xf605,
            0xf628, 0xf628,
            0xf68f, 0xf68f,
            0xf6a9, 0xf6a9,
            0
        };
        _renderer->merge_font("font/fluent icons.ttf", 16.0f, _icon_ranges);
        _ctx.fonts.track_title = _renderer->add_font("font/new rodin.otf", 14.0f);
        _ctx.fonts.subtitle = _renderer->add_font("font/aktiv grotesk.ttf", 16.0f);
        ImGui::GetIO().FontDefault = _ctx.fonts.ui;
        if (_ctx.store.config().library_path.empty()) {
            open_settings(_ctx);
        }
    } catch (...) {
        _window.reset();
        glfwTerminate();
        throw;
    }
}

window::~window()
{
    _media_transport.reset();
    _ctx.covers.release_textures();
    _renderer.reset();
    _window.reset();
    glfwTerminate();
}

void window::run()
{
    while (glfwWindowShouldClose(_window.get()) == GLFW_FALSE) {
        glfwPollEvents();

        _renderer->begin_frame();
        draw_app(_ctx);
        if (_media_transport) {
            _media_transport->update(_ctx);
        }
        _renderer->render(_ctx.player.status());

        glfwSwapBuffers(_window.get());
    }
}

}
