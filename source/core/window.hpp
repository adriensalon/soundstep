#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <core/renderer.hpp>

struct GLFWwindow;
#ifdef __ANDROID__
struct android_app;
#endif

namespace soundstep {

struct context;
struct system_media_transport;

struct window_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct window {
#ifdef __ANDROID__
    window(context& ctx, android_app* app);
#else
    explicit window(context& ctx);
#endif
    window(const window& other) = delete;
    window& operator=(const window& other) = delete;
    window(window&& other) = delete;
    window& operator=(window&& other) = delete;
    ~window();

    void run();

private:
    context& _ctx;
#ifdef __ANDROID__
    struct android_state;
    void _initialize_native_window();
    void _destroy_native_window() noexcept;
    void _handle_app_command(std::int32_t command);

    android_app* _app { nullptr };
    std::unique_ptr<android_state> _state;
#else
    std::shared_ptr<GLFWwindow> _window { nullptr };
#endif
    std::unique_ptr<renderer> _renderer { nullptr };
#ifndef __ANDROID__
    std::unique_ptr<system_media_transport> _media_transport { nullptr };
#endif
};

}
