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

#include <core/audio.hpp>
#include <core/config.hpp>
#include <core/security.hpp>

namespace soundstep {

struct storage_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct instance_info {
    std::string id;
    std::string name;
};

enum struct peer_origin {
    lan,
    pairing_code
};

enum struct peer_endpoint_family {
    ipv4,
    ipv6
};

struct peer_endpoint {
    std::string host;
    std::uint16_t port { 0 };
    peer_endpoint_family family { peer_endpoint_family::ipv4 };
    std::uint64_t last_seen_ms { 0 };
    std::uint64_t last_success_ms { 0 };
};

struct peer_record {
    std::string id;
    std::string name;
    std::string token;
    std::string fingerprint;
    peer_origin origin { peer_origin::lan };
    std::uint64_t last_seen_ms { 0 };
    bool library_enabled { true };
    std::vector<peer_endpoint> endpoints;
};

struct track {
    std::string id;
    std::string catalog_id;
    std::string file_hash;
    std::string cover_hash;
    std::string cover_content_type;
    std::uint64_t cover_size_bytes { 0 };
    audio_extension extension { audio_extension::unknown };
    std::string title;
    std::string artist;
    std::string album;
    std::uint32_t track_number { 0 };
    std::uint64_t duration_ms { 0 };
    std::uint64_t size_bytes { 0 };
};

struct cover_art {
    std::string hash;
    std::string content_type;
    std::vector<unsigned char> bytes;
};

struct catalog_snapshot {
    std::string id;
    std::string owner_instance_id;
    std::string name;
    std::uint64_t revision { 0 };
    std::vector<track> tracks;
};

enum struct file_kind {
    external,
    managed
};

struct file_location {
    std::string hash;
    std::string path;
    audio_extension extension { audio_extension::unknown };
    file_kind kind { file_kind::external };
    std::uint64_t size_bytes { 0 };
};

struct music_storage_usage {
    std::uint64_t local_bytes { 0 };
    std::uint64_t downloaded_bytes { 0 };
};

struct library_scan_result {
    std::size_t files_found { 0 };
    std::size_t tracks_added { 0 };
    std::size_t tracks_removed { 0 };
    std::size_t files_failed { 0 };
};

enum struct library_scan_state {
    idle,
    scanning,
    completed,
    failed
};

struct library_scan_status {
    library_scan_state state { library_scan_state::idle };
    library_scan_result result;
    std::string error_message;
};

struct storage {
    storage(std::filesystem::path database_path, std::filesystem::path asset_directory);
    storage(const storage& other) = delete;
    storage& operator=(const storage& other) = delete;
    storage(storage&& other) = delete;
    storage& operator=(storage&& other) = delete;
    ~storage();

    [[nodiscard]] instance_info instance() const;
    [[nodiscard]] std::string lan_token() const;
    [[nodiscard]] std::string create_invitation_token();
    [[nodiscard]] std::string access_token_for_peer(std::string_view peer_id);
    [[nodiscard]] bool authorize_access(std::string_view token) const;
    void claim_invitation(std::string_view token, std::string_view peer_id);
    [[nodiscard]] std::optional<soundstep::transport_identity> transport_identity() const;
    void set_transport_identity(const soundstep::transport_identity& identity);
    void set_instance_name(std::string name);

    [[nodiscard]] configuration config() const;
    void set_config(const configuration& config);
    [[nodiscard]] library_scan_result scan_library();

    [[nodiscard]] std::optional<track> playback_selection() const;
    void set_playback_selection(const track& selected_track);

    [[nodiscard]] std::vector<peer_record> peers() const;
    void upsert_peer(const peer_record& peer);
    void mark_peer_endpoint_success(std::string_view id, const peer_endpoint& endpoint);
    void set_peer_library_enabled(std::string_view id, bool enabled);
    void remove_peer(std::string_view id);

    [[nodiscard]] std::optional<std::uint64_t> catalog_revision(std::string_view id) const;
    [[nodiscard]] std::optional<catalog_snapshot> catalog(std::string_view id) const;
    void replace_catalog(const catalog_snapshot& catalog);
    void add_track(const track& track);
    void remove_track(std::string_view catalog_id, std::string_view track_id);
    void update_track_metadata(std::string_view catalog_id, std::string_view track_id, std::string title, std::string artist, std::string album, std::uint32_t track_number);

    [[nodiscard]] std::optional<file_location> find_file(std::string_view hash) const;
    [[nodiscard]] std::vector<file_location> managed_files() const;
    [[nodiscard]] music_storage_usage music_usage() const;
    [[nodiscard]] std::optional<cover_art> cover(std::string_view hash) const;
    void store_cover(const cover_art& cover);
    void register_external_file(const file_location& file);
    [[nodiscard]] std::vector<track> missing_files(std::string_view catalog_id) const;
    [[nodiscard]] std::vector<track> requested_offline_tracks() const;
    [[nodiscard]] bool track_offline_requested(std::string_view catalog_id, std::string_view track_id) const;
    void set_track_offline(std::string_view catalog_id, std::string_view track_id, bool offline);

    [[nodiscard]] std::filesystem::path partial_path(std::string_view hash) const;
    [[nodiscard]] std::filesystem::path managed_path(std::string_view hash, audio_extension extension) const;
    void commit_download(std::string_view hash, audio_extension extension, std::uint64_t expected_size);
    void remove_managed_file(std::string_view hash);

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation { nullptr };
};

struct library_scanner {
    explicit library_scanner(storage& store);
    library_scanner(const library_scanner& other) = delete;
    library_scanner& operator=(const library_scanner& other) = delete;
    library_scanner(library_scanner&& other) = delete;
    library_scanner& operator=(library_scanner&& other) = delete;
    ~library_scanner();

    void scan();
    [[nodiscard]] library_scan_status status() const;

private:
    struct implementation;
    std::unique_ptr<implementation> _implementation { nullptr };
};

}
