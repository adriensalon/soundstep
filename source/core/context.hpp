#pragma once

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <core/cover.hpp>
#include <core/offline.hpp>
#include <core/peer.hpp>
#include <core/playback.hpp>
#include <core/storage.hpp>

struct ImFont;

namespace soundstep {

struct widget_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct widget_fonts {
    ImFont* ui { nullptr };
    ImFont* brand { nullptr };
    ImFont* track_title { nullptr };
    ImFont* subtitle { nullptr };
};

enum struct widget_message_kind {
    progress,
    error
};

struct widget_message {
    std::string text;
    widget_message_kind kind { widget_message_kind::progress };
    std::chrono::steady_clock::time_point expires_at;
};

struct context {
    storage& store;
    library_scanner& scanner;
    playback& player;
    peer_server& server;
    peer_network& network;
    offline_service& offline;
    cover_cache& covers;
    widget_fonts fonts;
    std::optional<track> current_track;
    widget_message message;

    void notify(std::string text, widget_message_kind kind = widget_message_kind::progress, std::chrono::milliseconds duration = std::chrono::seconds(4));
    [[nodiscard]] const widget_message* active_message() noexcept;

    template <typename Operation>
    bool try_action(Operation&& operation) noexcept
    {
        try {
            std::forward<Operation>(operation)();
            return true;
        } catch (const std::exception& _exception) {
            notify(_exception.what(), widget_message_kind::error);
        } catch (...) {
            notify("Unknown application error", widget_message_kind::error);
        }
        return false;
    }
};

}
