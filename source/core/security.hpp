#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace soundstep {

struct security_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct transport_identity {
    std::string certificate_pem { };
    std::string private_key_pem { };
    std::string fingerprint { };
};

[[nodiscard]] transport_identity create_transport_identity();
[[nodiscard]] std::string data_fingerprint(std::string_view bytes);
[[nodiscard]] std::string file_fingerprint(const std::filesystem::path& path);
[[nodiscard]] std::string transport_certificate_fingerprint(std::string_view certificate);

}
