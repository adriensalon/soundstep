#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include <core/renderer.hpp>

struct GLFWwindow;

namespace soundstep {

struct context;

struct window_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct window {
    explicit window(context& ctx);
    window(const window& other) = delete;
    window& operator=(const window& other) = delete;
    window(window&& other) = delete;
    window& operator=(window&& other) = delete;
    ~window();

    void run();

private:
    context& _ctx;
    std::shared_ptr<GLFWwindow> _window { nullptr };
    std::unique_ptr<renderer> _renderer { nullptr };
};

}
