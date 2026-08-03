#ifdef __ANDROID__
#include <EGL/egl.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <imgui.h>

#include <backends/imgui_impl_android.h>

#include <stdexcept>

#include <core/context.hpp>
#include <core/window.hpp>
#include <widget/app.hpp>
#include <widget/settings.hpp>

namespace soundstep {
namespace {

    void _load_fonts(context& ctx, renderer& renderer)
    {
        ctx.fonts.ui = renderer.add_font("font/pp fraktion.otf", 16.0f);
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
        renderer.merge_font("font/fluent icons.ttf", 16.0f, ctx.fonts.ui, _icon_ranges);
        ctx.fonts.brand = renderer.add_font("font/astral delight.ttf", 26.0f);
        ctx.fonts.track_title = renderer.add_font("font/new rodin.otf", 14.0f);
        ctx.fonts.subtitle = renderer.add_font("font/aktiv grotesk.ttf", 16.0f);
        ImGui::GetIO().FontDefault = ctx.fonts.ui;
    }

}

struct window::android_state {
    EGLDisplay display { EGL_NO_DISPLAY };
    EGLSurface surface { EGL_NO_SURFACE };
    EGLContext context { EGL_NO_CONTEXT };
    bool settings_opened { false };
};

window::window(context& ctx, android_app* app)
    : _ctx(ctx)
    , _app(app)
    , _state(std::make_unique<android_state>())
{
    if (_app == nullptr) {
        throw window_error("Android application state is null");
    }
}

window::~window()
{
    _destroy_native_window();
}

void window::_initialize_native_window()
{
    if (_app->window == nullptr || _renderer) {
        return;
    }

    const EGLint _config_attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    const EGLint _context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    _state->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (_state->display == EGL_NO_DISPLAY || eglInitialize(_state->display, nullptr, nullptr) != EGL_TRUE || eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        _destroy_native_window();
        throw window_error("Failed to initialize Android EGL");
    }

    EGLConfig _config = nullptr;
    EGLint _config_count = 0;
    if (eglChooseConfig(_state->display, _config_attributes, &_config, 1, &_config_count) != EGL_TRUE || _config_count == 0) {
        _destroy_native_window();
        throw window_error("Android EGL did not provide an OpenGL ES 3 configuration");
    }

    EGLint _format = 0;
    eglGetConfigAttrib(_state->display, _config, EGL_NATIVE_VISUAL_ID, &_format);
    ANativeWindow_setBuffersGeometry(_app->window, 0, 0, _format);
    _state->surface = eglCreateWindowSurface(_state->display, _config, _app->window, nullptr);
    _state->context = eglCreateContext(_state->display, _config, EGL_NO_CONTEXT, _context_attributes);
    if (_state->surface == EGL_NO_SURFACE || _state->context == EGL_NO_CONTEXT || eglMakeCurrent(_state->display, _state->surface, _state->surface, _state->context) != EGL_TRUE) {
        _destroy_native_window();
        throw window_error("Failed to create the Android EGL surface or context");
    }
    eglSwapInterval(_state->display, 1);

    try {
        _renderer = std::make_unique<renderer>(_app->window);
        _load_fonts(_ctx, *_renderer);
        if (!_state->settings_opened && _ctx.store.config().library_path.empty()) {
            open_settings(_ctx);
            _state->settings_opened = true;
        }
    } catch (...) {
        _destroy_native_window();
        throw;
    }
}

void window::_destroy_native_window() noexcept
{
    if (_renderer) {
        _ctx.covers.release_textures();
        _renderer.reset();
    }
    _ctx.fonts = { };

    if (_state && _state->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(_state->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (_state->context != EGL_NO_CONTEXT) {
            eglDestroyContext(_state->display, _state->context);
        }
        if (_state->surface != EGL_NO_SURFACE) {
            eglDestroySurface(_state->display, _state->surface);
        }
        eglTerminate(_state->display);
    }
    if (_state) {
        _state->display = EGL_NO_DISPLAY;
        _state->surface = EGL_NO_SURFACE;
        _state->context = EGL_NO_CONTEXT;
    }
}

void window::_handle_app_command(std::int32_t command)
{
    switch (command) {
    case APP_CMD_INIT_WINDOW:
        _initialize_native_window();
        break;
    case APP_CMD_TERM_WINDOW:
    case APP_CMD_DESTROY:
        _destroy_native_window();
        break;
    default:
        break;
    }
}

void window::run()
{
    _app->userData = this;
    _app->onAppCmd = [](android_app* app, std::int32_t command) {
        window* _window = static_cast<window*>(app->userData);
        if (_window == nullptr) {
            return;
        }
        try {
            _window->_handle_app_command(command);
        } catch (const std::exception& exception) {
            __android_log_print(ANDROID_LOG_ERROR, "soundstep", "Android window error: %s", exception.what());
            ANativeActivity_finish(app->activity);
        }
    };
    _app->onInputEvent = [](android_app* app, AInputEvent* event) -> std::int32_t {
        window* _window = static_cast<window*>(app->userData);
        return _window != nullptr && _window->_renderer ? ImGui_ImplAndroid_HandleInputEvent(event) : 0;
    };

    while (_app->destroyRequested == 0) {
        int _events = 0;
        android_poll_source* _source = nullptr;
        int _identifier = 0;
        while ((_identifier = ALooper_pollOnce(_renderer ? 0 : -1, nullptr, &_events, reinterpret_cast<void**>(&_source))) >= 0) {
            if (_source != nullptr) {
                _source->process(_app, _source);
            }
            if (_app->destroyRequested != 0) {
                break;
            }
        }

        if (_renderer && _app->window != nullptr) {
            _renderer->begin_frame();
            draw_app(_ctx, nullptr);
            _renderer->render(_ctx.player.status());
            eglSwapBuffers(_state->display, _state->surface);
        }
    }
    _destroy_native_window();
}

}
#else
#include <limits>
#include <stdexcept>

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <cmrc/cmrc.hpp>
#include <imgui.h>
#include <stb_image.h>

#include <core/context.hpp>
#include <core/integration.hpp>
#include <core/window.hpp>
#include <widget/app.hpp>
#include <widget/settings.hpp>

CMRC_DECLARE(soundstep_resource);

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
        glfwGetMonitorWorkarea(_monitor, &_monitor_x, &_monitor_y, &_monitor_width, &_monitor_height);
        glfwGetWindowSize(window, &_window_width, &_window_height);
        glfwSetWindowPos(window, _monitor_x + (_monitor_width - _window_width) / 2, _monitor_y + (_monitor_height - _window_height) / 2);
    }

    void _set_window_icon(GLFWwindow* window)
    {
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            return;
        }

        try {
            constexpr const char* _icon_path = "platform/common/soundstep.png";
            const cmrc::embedded_filesystem _resources = cmrc::soundstep_resource::get_filesystem();
            if (!_resources.is_file(_icon_path)) {
                return;
            }

            const cmrc::file _data = _resources.open(_icon_path);
            const std::size_t _data_size = static_cast<std::size_t>(_data.end() - _data.begin());
            if (_data_size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
                return;
            }

            int _width = 0;
            int _height = 0;
            int _channels = 0;
            unsigned char* _pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(_data.begin()), static_cast<int>(_data_size), &_width, &_height, &_channels, 4);
            if (_pixels == nullptr || _width <= 0 || _height <= 0) {
                stbi_image_free(_pixels);
                return;
            }

            const GLFWimage _icon { _width, _height, _pixels };
            glfwSetWindowIcon(window, 1, &_icon);
            stbi_image_free(_pixels);
        } catch (...) {
            // TODO : a missing icon must not prevent the application from starting
        }
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
#endif
