#pragma once

#include <memory>
#include <stdexcept>

#ifdef __ANDROID__
struct android_app;
#endif

namespace soundstep {

struct context;

struct window_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct window {
#ifdef __ANDROID__
    explicit window(context& ctx, android_app* app);
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
    struct implementation;
    std::unique_ptr<implementation> _implementation { nullptr };
};

}
