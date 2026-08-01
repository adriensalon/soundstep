#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>

#include <core/audio.hpp>

namespace soundstep {

struct playback_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum struct playback_state {
    stopped,
    buffering,
    playing,
    paused,
    finished,
    failed
};

struct playback_status {
    playback_state state { playback_state::stopped };
    bool has_source { false };
    double position_seconds { 0.0 };
    double duration_seconds { 0.0 };
    double buffered_seconds { 0.0 };
};

struct playback {
    playback();
    playback(const playback& other) = delete;
    playback& operator=(const playback& other) = delete;
    playback(playback&& other) noexcept;
    playback& operator=(playback&& other) noexcept;
    ~playback();

    void open(std::shared_ptr<audio_source> source, audio_extension extension);
    void open(const std::filesystem::path& path);
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    void set_volume(float volume);
    [[nodiscard]] playback_status status() const;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation { nullptr };
};

}
