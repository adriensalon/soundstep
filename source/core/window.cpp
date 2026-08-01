#include <stdexcept>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <core/context.hpp>
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
        _ctx.fonts.body = _renderer->add_font("font/new rodin.otf", 16.0f);
        _ctx.fonts.heading = _renderer->add_font("font/new rodin.otf", 24.0f);
        _ctx.fonts.emphasis = _renderer->add_font("font/aktivgrotesk italic.ttf", 16.0f);
        ImGui::GetIO().FontDefault = _ctx.fonts.body;
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
        _renderer->render();

        glfwSwapBuffers(_window.get());
    }
}

}
