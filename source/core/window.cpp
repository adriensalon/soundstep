#include <algorithm>
#include <stdexcept>
#include <vector>

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
namespace {

    void _center_window(GLFWwindow* window)
    {
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            return;
        }

        GLFWmonitor* _monitor = glfwGetPrimaryMonitor();
        if (_monitor == nullptr) {
            return;
        }

        int _monitor_x = 0;
        int _monitor_y = 0;
        int _monitor_width = 0;
        int _monitor_height = 0;
        int _window_width = 0;
        int _window_height = 0;
        glfwGetMonitorWorkarea(
            _monitor,
            &_monitor_x,
            &_monitor_y,
            &_monitor_width,
            &_monitor_height);
        glfwGetWindowSize(window, &_window_width, &_window_height);
        glfwSetWindowPos(
            window,
            _monitor_x + (_monitor_width - _window_width) / 2,
            _monitor_y + (_monitor_height - _window_height) / 2);
    }

    void _set_window_icon(GLFWwindow* window)
    {
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            return;
        }

        constexpr int _size = 32;
        std::vector<unsigned char> _pixels(_size * _size * 4, 0);
        for (int _y = 0; _y < _size; ++_y) {
            for (int _x = 0; _x < _size; ++_x) {
                const int _edge_x = (std::min)(_x, _size - 1 - _x);
                const int _edge_y = (std::min)(_y, _size - 1 - _y);
                const bool _inside = _edge_x >= 3 && _edge_y >= 3
                    && !((_edge_x == 3 || _edge_y == 3) && _edge_x + _edge_y < 8);
                if (!_inside) {
                    continue;
                }

                const std::size_t _offset = static_cast<std::size_t>((_y * _size + _x) * 4);
                const bool _bar = (_x >= 9 && _x <= 11 && _y >= 15 && _y <= 23)
                    || (_x >= 15 && _x <= 17 && _y >= 9 && _y <= 23)
                    || (_x >= 21 && _x <= 23 && _y >= 13 && _y <= 23);
                const unsigned char _value = _bar ? 238 : 64;
                _pixels[_offset] = _value;
                _pixels[_offset + 1] = _value;
                _pixels[_offset + 2] = _value;
                _pixels[_offset + 3] = 255;
            }
        }

        GLFWimage _icon { _size, _size, _pixels.data() };
        glfwSetWindowIcon(window, 1, &_icon);
    }

}

window::window(context& ctx)
    : _ctx(ctx)
{
    if (glfwInit() != GLFW_TRUE) {
        throw window_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* _native_window = glfwCreateWindow(1280, 720, "SoundStep", nullptr, nullptr);
    if (_native_window == nullptr) {
        glfwTerminate();
        throw window_error("Failed to create the SoundStep window");
    }

    _window = std::shared_ptr<GLFWwindow>(_native_window, glfwDestroyWindow);
    glfwSetWindowSizeLimits(_native_window, 760, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    _center_window(_native_window);
    _set_window_icon(_native_window);
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
        _renderer->merge_font("font/fluent icons.ttf", 16.0f, _ctx.fonts.ui, _icon_ranges);
        _ctx.fonts.brand = _renderer->add_font("font/astral delight.ttf", 26.0f);
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

    glfwShowWindow(_native_window);
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
        draw_app(_ctx, _window.get());
        if (_media_transport) {
            _media_transport->update(_ctx);
        }
        _renderer->render(_ctx.player.status());

        glfwSwapBuffers(_window.get());
    }
}

}
