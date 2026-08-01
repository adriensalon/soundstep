#pragma once

#include <memory>

namespace soundstep {

struct context;

struct system_media_transport {
    explicit system_media_transport(void* native_window);
    system_media_transport(const system_media_transport& other) = delete;
    system_media_transport& operator=(const system_media_transport& other) = delete;
    system_media_transport(system_media_transport&& other) = delete;
    system_media_transport& operator=(system_media_transport&& other) = delete;
    ~system_media_transport();

    void update(context& ctx) noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation;
};

}
