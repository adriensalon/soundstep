#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace soundstep {

struct peer_client;

struct audio_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum struct audio_extension : std::uint8_t {
    unknown,
    mp3,
    wav,
    flac,
    ogg
};

[[nodiscard]] std::string_view audio_extension_name(audio_extension extension) noexcept;
[[nodiscard]] int audio_extension_priority(audio_extension extension) noexcept;
[[nodiscard]] std::optional<audio_extension> parse_audio_extension(std::string_view extension) noexcept;
[[nodiscard]] std::optional<audio_extension> audio_extension_from_path(const std::filesystem::path& path) noexcept;
[[nodiscard]] std::string_view audio_content_type(audio_extension extension) noexcept;

struct audio_metadata {
    std::string title { };
    std::string artist { };
    std::string album { };
    std::uint32_t track_number { 0 };
    std::uint64_t duration_ms { 0 };
    std::string cover_content_type { };
    std::vector<unsigned char> cover_bytes { };
};

[[nodiscard]] audio_metadata inspect_audio_file(const std::filesystem::path& path, audio_extension extension);

struct audio_source {
    virtual ~audio_source() = default;

    virtual std::size_t read(std::byte* destination, std::size_t size) = 0;
    virtual bool seek(std::uint64_t offset) = 0;
    [[nodiscard]] virtual std::uint64_t tell() const = 0;
    [[nodiscard]] virtual std::uint64_t size() const = 0;
};

struct file_audio_source final : audio_source {
    explicit file_audio_source(std::filesystem::path path);
    file_audio_source(const file_audio_source& other) = delete;
    file_audio_source& operator=(const file_audio_source& other) = delete;
    file_audio_source(file_audio_source&& other) noexcept;
    file_audio_source& operator=(file_audio_source&& other) noexcept;
    ~file_audio_source() override;

    std::size_t read(std::byte* destination, std::size_t size) override;
    bool seek(std::uint64_t offset) override;
    [[nodiscard]] std::uint64_t tell() const override;
    [[nodiscard]] std::uint64_t size() const override;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation { nullptr };
};

struct stream_audio_source final : audio_source {
    stream_audio_source(std::shared_ptr<peer_client> client, std::string hash, std::uint64_t size, std::size_t read_ahead_size = 256 * 1024);
    stream_audio_source(const stream_audio_source& other) = delete;
    stream_audio_source& operator=(const stream_audio_source& other) = delete;
    stream_audio_source(stream_audio_source&& other) noexcept;
    stream_audio_source& operator=(stream_audio_source&& other) noexcept;
    ~stream_audio_source() override;

    std::size_t read(std::byte* destination, std::size_t size) override;
    bool seek(std::uint64_t offset) override;
    [[nodiscard]] std::uint64_t tell() const override;
    [[nodiscard]] std::uint64_t size() const override;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation { nullptr };
};

}
