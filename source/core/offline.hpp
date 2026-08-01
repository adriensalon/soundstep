#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace soundstep {

struct storage;
struct track;

struct offline_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum struct offline_state {
    off,
    downloading,
    on,
    failed
};

struct offline_status {
    offline_state state { offline_state::off };
    std::uint64_t downloaded_bytes { 0 };
    std::uint64_t total_bytes { 0 };
    bool can_remove { false };
    bool requested { false };
    std::string error_message;
};

struct offline_service {
    explicit offline_service(storage& store);
    offline_service(const offline_service& other) = delete;
    offline_service& operator=(const offline_service& other) = delete;
    offline_service(offline_service&& other) = delete;
    offline_service& operator=(offline_service&& other) = delete;
    ~offline_service();

    void save(const track& track);
    void remove(const track& track);
    void reconcile();
    [[nodiscard]] offline_status status(const track& track) const;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation;
};

}
