#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <core/storage.hpp>

namespace soundstep {

inline constexpr std::uint32_t protocol_current_version = 1;
inline constexpr std::string_view protocol_content_type = "application/vnd.soundstep+cereal";
inline constexpr std::size_t protocol_maximum_payload_size = 16 * 1024 * 1024;
inline constexpr std::size_t protocol_maximum_track_count = 100'000;
inline constexpr std::size_t protocol_maximum_endpoint_count = 32;

struct protocol_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct peer_discovery {
    instance_info instance { };
    std::vector<peer_endpoint> endpoints { };
    std::string token { };
    std::string fingerprint { };
};

struct peer_invite {
    instance_info instance { };
    std::vector<peer_endpoint> endpoints { };
    std::string token { };
    std::string fingerprint { };
};

[[nodiscard]] std::string protocol_encode_instance(const instance_info& instance);
[[nodiscard]] instance_info protocol_decode_instance(std::string_view bytes);
[[nodiscard]] std::string protocol_encode_catalog(const catalog_snapshot& catalog);
[[nodiscard]] catalog_snapshot protocol_decode_catalog(std::string_view bytes);
[[nodiscard]] std::string protocol_encode_discovery(const peer_discovery& discovery);
[[nodiscard]] peer_discovery protocol_decode_discovery(std::string_view bytes);
[[nodiscard]] std::string protocol_encode_invite(const peer_invite& invite);
[[nodiscard]] peer_invite protocol_decode_invite(std::string_view code);

}
