#ifdef __ANDROID__
#include <EGL/egl.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <imgui.h>
#include <jni.h>

#include <backends/imgui_impl_android.h>

#include <cstdint>
#include <stdexcept>

#include <core/context.hpp>
#include <core/renderer.hpp>
#include <core/window.hpp>
#include <view/app.hpp>
#include <view/settings.hpp>

namespace soundstep {
namespace {

    void _apply_android_content_rect(const android_app& app)
    {
        const ARect& _content = app.contentRect;
        if (_content.right <= _content.left || _content.bottom <= _content.top) {
            return;
        }

        ImGuiViewport* _viewport = ImGui::GetMainViewport();
        _viewport->WorkPos = ImVec2(static_cast<float>(_content.left), static_cast<float>(_content.top));
        _viewport->WorkSize = ImVec2(static_cast<float>(_content.right - _content.left), static_cast<float>(_content.bottom - _content.top));
    }

    void _load_fonts(context& ctx, renderer& renderer)
    {
        ctx.fonts.ui = renderer.add_font("font/pp fraktion.otf", 18.0f);
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
        renderer.merge_font("font/fluent icons.ttf", 18.0f, ctx.fonts.ui, _icon_ranges);
        ctx.fonts.brand = renderer.add_font("font/astral delight.ttf", 28.0f);
        ctx.fonts.track_title = renderer.add_font("font/new rodin.otf", 16.0f);
        ctx.fonts.subtitle = renderer.add_font("font/aktiv grotesk.ttf", 18.0f);
        ImGui::GetIO().FontDefault = ctx.fonts.ui;
    }

}

struct window::implementation {
    implementation(context& ctx, android_app* app);
    ~implementation();

    void initialize_native_window();
    void destroy_native_window() noexcept;
    void handle_app_command(std::int32_t command);
    void poll_android_input();
    void update_soft_keyboard();
    void run();

    struct state {
        EGLDisplay display { EGL_NO_DISPLAY };
        EGLSurface surface { EGL_NO_SURFACE };
        EGLContext context { EGL_NO_CONTEXT };
        bool settings_opened { false };
    };

    context& _ctx;
    android_app* _app { nullptr };
    state _state;
    std::unique_ptr<renderer> _renderer { nullptr };
    JNIEnv* _java_environment { nullptr };
    jmethodID _show_soft_input { nullptr };
    jmethodID _hide_soft_input { nullptr };
    jmethodID _poll_input_event { nullptr };
    bool _java_thread_attached { false };
    bool _soft_keyboard_visible { false };
};

window::implementation::implementation(context& ctx, android_app* app)
    : _ctx(ctx)
    , _app(app)
{
    if (_app == nullptr) {
        throw window_error("Android application state is null");
    }

    JavaVM* _java_vm = _app->activity->vm;
    const jint _environment_status = _java_vm->GetEnv(reinterpret_cast<void**>(&_java_environment), JNI_VERSION_1_6);
    if (_environment_status == JNI_EDETACHED) {
        if (_java_vm->AttachCurrentThread(&_java_environment, nullptr) != JNI_OK) {
            _java_environment = nullptr;
            return;
        }
        _java_thread_attached = true;
    } else if (_environment_status != JNI_OK) {
        _java_environment = nullptr;
        return;
    }

    jclass _activity_class = _java_environment->GetObjectClass(_app->activity->clazz);
    if (_activity_class != nullptr) {
        _show_soft_input = _java_environment->GetMethodID(_activity_class, "showSoftInput", "()V");
        _hide_soft_input = _java_environment->GetMethodID(_activity_class, "hideSoftInput", "()V");
        _poll_input_event = _java_environment->GetMethodID(_activity_class, "pollInputEvent", "()I");
        _java_environment->DeleteLocalRef(_activity_class);
    }
    if (_java_environment->ExceptionCheck()) {
        _java_environment->ExceptionClear();
        _show_soft_input = nullptr;
        _hide_soft_input = nullptr;
        _poll_input_event = nullptr;
    }
}

window::implementation::~implementation()
{
    destroy_native_window();
    if (_java_thread_attached) {
        _app->activity->vm->DetachCurrentThread();
    }
}

void window::implementation::initialize_native_window()
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

    _state.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (_state.display == EGL_NO_DISPLAY || eglInitialize(_state.display, nullptr, nullptr) != EGL_TRUE || eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        destroy_native_window();
        throw window_error("Failed to initialize Android EGL");
    }

    EGLConfig _config = nullptr;
    EGLint _config_count = 0;
    if (eglChooseConfig(_state.display, _config_attributes, &_config, 1, &_config_count) != EGL_TRUE || _config_count == 0) {
        destroy_native_window();
        throw window_error("Android EGL did not provide an OpenGL ES 3 configuration");
    }

    EGLint _format = 0;
    eglGetConfigAttrib(_state.display, _config, EGL_NATIVE_VISUAL_ID, &_format);
    ANativeWindow_setBuffersGeometry(_app->window, 0, 0, _format);
    _state.surface = eglCreateWindowSurface(_state.display, _config, _app->window, nullptr);
    _state.context = eglCreateContext(_state.display, _config, EGL_NO_CONTEXT, _context_attributes);
    if (_state.surface == EGL_NO_SURFACE || _state.context == EGL_NO_CONTEXT || eglMakeCurrent(_state.display, _state.surface, _state.surface, _state.context) != EGL_TRUE) {
        destroy_native_window();
        throw window_error("Failed to create the Android EGL surface or context");
    }
    eglSwapInterval(_state.display, 1);

    try {
        _renderer = std::make_unique<renderer>(_app->window);
        _load_fonts(_ctx, *_renderer);
        if (!_state.settings_opened && _ctx.store.config().library_path.empty()) {
            open_settings(_ctx);
            _state.settings_opened = true;
        }
    } catch (...) {
        destroy_native_window();
        throw;
    }
}

void window::implementation::destroy_native_window() noexcept
{
    if (_renderer) {
        _ctx.covers.release_textures();
        _renderer.reset();
    }
    _ctx.fonts = { };

    if (_state.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(_state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (_state.context != EGL_NO_CONTEXT) {
            eglDestroyContext(_state.display, _state.context);
        }
        if (_state.surface != EGL_NO_SURFACE) {
            eglDestroySurface(_state.display, _state.surface);
        }
        eglTerminate(_state.display);
    }
    _state.display = EGL_NO_DISPLAY;
    _state.surface = EGL_NO_SURFACE;
    _state.context = EGL_NO_CONTEXT;
}

void window::implementation::handle_app_command(std::int32_t command)
{
    switch (command) {
    case APP_CMD_INIT_WINDOW:
        initialize_native_window();
        break;
    case APP_CMD_TERM_WINDOW:
    case APP_CMD_DESTROY:
        destroy_native_window();
        break;
    case APP_CMD_LOST_FOCUS:
        if (_soft_keyboard_visible && _java_environment != nullptr && _hide_soft_input != nullptr) {
            _java_environment->CallVoidMethod(_app->activity->clazz, _hide_soft_input);
            _soft_keyboard_visible = false;
        }
        break;
    default:
        break;
    }
}

void window::implementation::poll_android_input()
{
    if (_java_environment == nullptr || _poll_input_event == nullptr) {
        return;
    }

    ImGuiIO& _io = ImGui::GetIO();
    for (;;) {
        const jint _event = _java_environment->CallIntMethod(_app->activity->clazz, _poll_input_event);
        if (_java_environment->ExceptionCheck()) {
            _java_environment->ExceptionClear();
            return;
        }
        if (_event == 0) {
            return;
        }
        if (_event == -1) {
            _io.AddKeyEvent(ImGuiKey_Backspace, true);
            _io.AddKeyEvent(ImGuiKey_Backspace, false);
        } else if (_event == -2) {
            _io.AddKeyEvent(ImGuiKey_Enter, true);
            _io.AddKeyEvent(ImGuiKey_Enter, false);
        } else if (_event > 0) {
            _io.AddInputCharacter(static_cast<unsigned int>(_event));
        }
    }
}

void window::implementation::update_soft_keyboard()
{
    if (_java_environment == nullptr || _show_soft_input == nullptr || _hide_soft_input == nullptr) {
        return;
    }

    const bool _keyboard_requested = ImGui::GetIO().WantTextInput;
    if (_keyboard_requested == _soft_keyboard_visible) {
        return;
    }
    _java_environment->CallVoidMethod(
        _app->activity->clazz,
        _keyboard_requested ? _show_soft_input : _hide_soft_input);
    if (_java_environment->ExceptionCheck()) {
        _java_environment->ExceptionClear();
        return;
    }
    _soft_keyboard_visible = _keyboard_requested;
}

void window::implementation::run()
{
    _app->userData = this;
    _app->onAppCmd = [](android_app* app, std::int32_t command) {
        implementation* _window = static_cast<implementation*>(app->userData);
        if (_window == nullptr) {
            return;
        }
        try {
            _window->handle_app_command(command);
        } catch (const std::exception& exception) {
            __android_log_print(ANDROID_LOG_ERROR, "soundstep", "Android window error: %s", exception.what());
            ANativeActivity_finish(app->activity);
        }
    };
    _app->onInputEvent = [](android_app* app, AInputEvent* event) -> std::int32_t {
        implementation* _window = static_cast<implementation*>(app->userData);
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
            poll_android_input();
            _renderer->begin_frame();
            _apply_android_content_rect(*_app);
            draw_app(_ctx, nullptr);
            update_soft_keyboard();
            _renderer->render(_ctx.player.status());
            eglSwapBuffers(_state.display, _state.surface);
        }
    }
    destroy_native_window();
}

window::window(context& ctx, android_app* app)
    : _implementation(std::make_unique<implementation>(ctx, app))
{
}

window::~window() = default;

void window::run()
{
    _implementation->run();
}

}

#else

#include <limits>
#include <stdexcept>

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <shellapi.h>

#include "../../platform/win32/resource.h"
#endif
#include <cmrc/cmrc.hpp>
#include <imgui.h>
#include <stb_image.h>

#include <core/context.hpp>
#include <core/integration.hpp>
#include <core/renderer.hpp>
#include <core/window.hpp>
#include <view/app.hpp>
#include <view/settings.hpp>

CMRC_DECLARE(soundstep_resource);

namespace soundstep {
namespace {

#ifdef _WIN32
    constexpr UINT _tray_callback_message = WM_APP + 1;
    constexpr UINT _tray_exit_command = 0x1001;
    constexpr UINT _tray_show_command = 0x1002;
    constexpr wchar_t _tray_window_property[] = L"SoundStep.WindowImplementation";
#endif

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

struct window::implementation {
    explicit implementation(context& ctx);
    ~implementation();

    void run();

#ifdef _WIN32
    static LRESULT CALLBACK window_procedure(HWND native_window, UINT message, WPARAM word_parameter, LPARAM long_parameter);
    void install_tray_icon();
    void remove_tray_icon() noexcept;
    void restore_from_tray();
    void show_tray_menu();
#endif

    context& _ctx;
    std::shared_ptr<GLFWwindow> _window { nullptr };
    std::unique_ptr<renderer> _renderer { nullptr };
    std::unique_ptr<system_media_transport> _media_transport { nullptr };
#ifdef _WIN32
    HWND _native_window { nullptr };
    WNDPROC _original_window_procedure { nullptr };
    NOTIFYICONDATAW _tray_icon { };
    bool _tray_icon_added { false };
    bool _exit_requested { false };
#endif
};

#ifdef _WIN32
LRESULT CALLBACK window::implementation::window_procedure(HWND native_window, UINT message, WPARAM word_parameter, LPARAM long_parameter)
{
    implementation* _self = reinterpret_cast<implementation*>(GetPropW(native_window, _tray_window_property));
    if (_self != nullptr) {
        if (message == _tray_callback_message) {
            switch (static_cast<UINT>(long_parameter)) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                _self->restore_from_tray();
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                _self->show_tray_menu();
                break;
            default:
                break;
            }
            return 0;
        }
        if (message == WM_COMMAND) {
            switch (LOWORD(word_parameter)) {
            case _tray_show_command:
                _self->restore_from_tray();
                return 0;
            case _tray_exit_command:
                _self->_exit_requested = true;
                glfwSetWindowShouldClose(_self->_window.get(), GLFW_TRUE);
                return 0;
            default:
                break;
            }
        }
        return CallWindowProcW(_self->_original_window_procedure, native_window, message, word_parameter, long_parameter);
    }
    return DefWindowProcW(native_window, message, word_parameter, long_parameter);
}

void window::implementation::install_tray_icon()
{
    if (_native_window == nullptr || SetPropW(_native_window, _tray_window_property, reinterpret_cast<HANDLE>(this)) == FALSE) {
        throw window_error("Failed to initialize the SoundStep tray icon");
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR _previous = SetWindowLongPtrW(_native_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_procedure));
    if (_previous == 0 && GetLastError() != ERROR_SUCCESS) {
        RemovePropW(_native_window, _tray_window_property);
        throw window_error("Failed to receive SoundStep tray icon events");
    }
    _original_window_procedure = reinterpret_cast<WNDPROC>(_previous);

    _tray_icon.cbSize = sizeof(_tray_icon);
    _tray_icon.hWnd = _native_window;
    _tray_icon.uID = 1;
    _tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    _tray_icon.uCallbackMessage = _tray_callback_message;
    _tray_icon.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_SOUNDSTEP_ICON));
    if (_tray_icon.hIcon == nullptr) {
        _tray_icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }
    lstrcpynW(_tray_icon.szTip, L"SoundStep", static_cast<int>(std::size(_tray_icon.szTip)));

    if (Shell_NotifyIconW(NIM_ADD, &_tray_icon) == FALSE) {
        remove_tray_icon();
        throw window_error("Failed to add the SoundStep tray icon");
    }
    _tray_icon_added = true;
}

void window::implementation::remove_tray_icon() noexcept
{
    if (_tray_icon_added) {
        Shell_NotifyIconW(NIM_DELETE, &_tray_icon);
        _tray_icon_added = false;
    }
    if (_native_window != nullptr && IsWindow(_native_window) != FALSE) {
        if (_original_window_procedure != nullptr) {
            SetWindowLongPtrW(_native_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(_original_window_procedure));
            _original_window_procedure = nullptr;
        }
        RemovePropW(_native_window, _tray_window_property);
    }
}

void window::implementation::restore_from_tray()
{
    glfwShowWindow(_window.get());
    glfwRestoreWindow(_window.get());
    glfwFocusWindow(_window.get());
}

void window::implementation::show_tray_menu()
{
    HMENU _menu = CreatePopupMenu();
    if (_menu == nullptr) {
        return;
    }
    AppendMenuW(_menu, MF_STRING, _tray_show_command, L"Show");
    AppendMenuW(_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(_menu, MF_STRING, _tray_exit_command, L"Exit");
    SetMenuDefaultItem(_menu, _tray_show_command, FALSE);

    POINT _cursor { };
    GetCursorPos(&_cursor);
    SetForegroundWindow(_native_window);
    TrackPopupMenu(_menu, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, _cursor.x, _cursor.y, 0, _native_window, nullptr);
    DestroyMenu(_menu);
    PostMessageW(_native_window, WM_NULL, 0, 0);
}
#endif

window::implementation::implementation(context& ctx)
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
#ifdef _WIN32
    this->_native_window = glfwGetWin32Window(_native_window);
    glfwSetWindowCloseCallback(_native_window, [](GLFWwindow* window) {
        glfwSetWindowShouldClose(window, GLFW_FALSE);
        glfwHideWindow(window);
    });
#endif
    glfwSetWindowSizeLimits(_native_window, 760, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    _center_window(_native_window);
    _set_window_icon(_native_window);
    glfwMakeContextCurrent(_native_window);
    glfwSwapInterval(1);

    try {
        _renderer = std::make_unique<renderer>(_window);
#ifdef _WIN32
        _media_transport = std::make_unique<system_media_transport>(this->_native_window);
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
#ifdef _WIN32
        install_tray_icon();
#endif
    } catch (...) {
#ifdef _WIN32
        remove_tray_icon();
#endif
        _media_transport.reset();
        _renderer.reset();
        _window.reset();
        glfwTerminate();
        throw;
    }

    glfwShowWindow(_native_window);
}

window::implementation::~implementation()
{
#ifdef _WIN32
    remove_tray_icon();
#endif
    _media_transport.reset();
    _ctx.covers.release_textures();
    _renderer.reset();
    _window.reset();
    glfwTerminate();
}

void window::implementation::run()
{
#ifdef _WIN32
    while (!_exit_requested) {
        if (glfwGetWindowAttrib(_window.get(), GLFW_VISIBLE) == GLFW_TRUE) {
            glfwPollEvents();
        } else {
            glfwWaitEventsTimeout(0.25);
        }

        if (_exit_requested) {
            break;
        }
        if (glfwWindowShouldClose(_window.get()) == GLFW_TRUE) {
            glfwSetWindowShouldClose(_window.get(), GLFW_FALSE);
            glfwHideWindow(_window.get());
            continue;
        }
        if (glfwGetWindowAttrib(_window.get(), GLFW_VISIBLE) == GLFW_FALSE) {
            if (_media_transport) {
                _media_transport->update(_ctx);
            }
            continue;
        }

        _renderer->begin_frame();
        draw_app(_ctx, _window.get());
        if (_media_transport) {
            _media_transport->update(_ctx);
        }
        _renderer->render(_ctx.player.status());

        glfwSwapBuffers(_window.get());
    }
#else
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
#endif
}

window::window(context& ctx)
    : _implementation(std::make_unique<implementation>(ctx))
{
}

window::~window() = default;

void window::run()
{
    _implementation->run();
}

}
#endif
