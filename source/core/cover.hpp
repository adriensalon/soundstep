#pragma once

#include <memory>
#include <optional>
#include <stdexcept>

#include <core/renderer.hpp>

namespace soundstep {

struct storage;
struct track;

struct cover_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct cover_cache {
    explicit cover_cache(storage& store);
    cover_cache(const cover_cache& other) = delete;
    cover_cache& operator=(const cover_cache& other) = delete;
    cover_cache(cover_cache&& other) = delete;
    cover_cache& operator=(cover_cache&& other) = delete;
    ~cover_cache();

    [[nodiscard]] std::optional<renderer_texture> texture(const track& track);
    void release_textures() noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation { nullptr };
};

}
