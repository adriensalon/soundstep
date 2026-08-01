#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace soundstep {

inline constexpr std::uint16_t peer_default_port = 51411;

struct storage;
struct instance_info;
struct peer_discovery;
struct peer_record;

struct peer_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct peer_response {
    int status_code { 0 };
    std::string body;
    std::string error_message;

    [[nodiscard]] bool ok() const noexcept;
    explicit operator bool() const noexcept;
};

struct peer_client {
    using chunk_handler = std::function<bool(const char* data, std::size_t size)>;

    peer_client(std::string host, std::uint16_t port, std::string token, std::string fingerprint);
    peer_client(storage& store, const peer_record& record);
    peer_client(const peer_client& other) = delete;
    peer_client& operator=(const peer_client& other) = delete;
    peer_client(peer_client&& other) noexcept;
    peer_client& operator=(peer_client&& other) noexcept;
    ~peer_client();

    [[nodiscard]] peer_response fetch_instance();
    [[nodiscard]] peer_response fetch_catalog();
    [[nodiscard]] peer_response fetch_cover(std::string_view hash);
    [[nodiscard]] peer_response announce(const peer_discovery& discovery);
    [[nodiscard]] peer_response stream_asset(std::string_view hash, const chunk_handler& on_chunk, std::uint64_t offset = 0, std::optional<std::uint64_t> length = std::nullopt);
    [[nodiscard]] peer_response download_asset(std::string_view hash, const std::filesystem::path& destination);
    [[nodiscard]] const std::string& host() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] const std::string& fingerprint() const noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation;
};

struct peer_server {
    peer_server(storage& store, std::uint16_t port = peer_default_port, std::string bind_address = "::");
    ~peer_server();

    peer_server(const peer_server& other) = delete;
    peer_server& operator=(const peer_server& other) = delete;
    peer_server(peer_server&& other) = delete;
    peer_server& operator=(peer_server&& other) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] const std::string& fingerprint() const noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation;
};

struct peer_network {
    peer_network(storage& store, peer_server& server);
    ~peer_network();

    peer_network(const peer_network& other) = delete;
    peer_network& operator=(const peer_network& other) = delete;
    peer_network(peer_network&& other) = delete;
    peer_network& operator=(peer_network&& other) = delete;

    [[nodiscard]] std::string create_pairing_code();
    void accept_pairing_code(std::string_view code);
    void refresh_catalogs();
    void refresh_catalogs_async() noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation;
};

}
