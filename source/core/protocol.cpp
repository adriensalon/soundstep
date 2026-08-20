#include <cstdint>
#include <limits>
#include <sstream>
#include <vector>

#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

#include <core/protocol.hpp>

namespace soundstep {
namespace {

    constexpr std::uint32_t message_magic = 0x53535450u; // SSTP
    constexpr std::size_t maximum_string_size = 1024 * 1024;

    enum struct message_kind : std::uint8_t {
        instance = 1,
        catalog = 2,
        discovery = 3,
        invite = 4
    };

    struct bounded_string {
        bounded_string() = default;
        explicit bounded_string(std::string text)
            : _value(std::move(text))
        {
            if (_value.size() > maximum_string_size) {
                throw protocol_error("Protocol string exceeds the size limit");
            }
        }

        template <typename Archive>
        void save(Archive& archive) const
        {
            const std::uint32_t _size = static_cast<std::uint32_t>(_value.size());
            archive(_size);
            if (_size != 0) {
                archive(cereal::binary_data(_value.data(), _size));
            }
        }

        template <typename Archive>
        void load(Archive& archive)
        {
            std::uint32_t _size = 0;
            archive(_size);
            if (_size > maximum_string_size) {
                throw protocol_error("Protocol string exceeds the size limit");
            }
            _value.resize(_size);
            if (_size != 0) {
                archive(cereal::binary_data(_value.data(), _size));
            }
        }

        std::string _value;
    };

    struct wire_track {
        bounded_string id;
        bounded_string file_hash;
        bounded_string cover_hash;
        bounded_string cover_content_type;
        std::uint64_t cover_size_bytes { 0 };
        bounded_string extension;
        bounded_string title;
        bounded_string artist;
        bounded_string album;
        std::uint32_t track_number { 0 };
        std::uint64_t duration_ms { 0 };
        std::uint64_t size_bytes { 0 };

        template <typename Archive>
        void serialize(Archive& archive)
        {
            archive(
                id,
                file_hash,
                cover_hash,
                cover_content_type,
                cover_size_bytes,
                extension,
                title,
                artist,
                album,
                track_number,
                duration_ms,
                size_bytes);
        }
    };

    struct instance_message {
        std::uint32_t magic { message_magic };
        std::uint32_t protocol_version { protocol_current_version };
        message_kind kind { message_kind::instance };
        bounded_string id;
        bounded_string name;

        template <typename Archive>
        void serialize(Archive& archive)
        {
            archive(magic, protocol_version, kind, id, name);
        }
    };

    struct catalog_message {
        std::uint32_t magic { message_magic };
        std::uint32_t protocol_version { protocol_current_version };
        message_kind kind { message_kind::catalog };
        bounded_string id;
        bounded_string owner_instance_id;
        bounded_string name;
        std::uint64_t revision { 0 };
        std::vector<wire_track> tracks;

        template <typename Archive>
        void save(Archive& archive) const
        {
            if (tracks.size() > protocol_maximum_track_count) {
                throw protocol_error("Catalog contains too many tracks");
            }

            archive(magic, protocol_version, kind, id, owner_instance_id, name, revision);
            const std::uint32_t _track_count = static_cast<std::uint32_t>(tracks.size());
            archive(_track_count);
            for (const wire_track& _track : tracks) {
                archive(_track);
            }
        }

        template <typename Archive>
        void load(Archive& archive)
        {
            archive(magic, protocol_version, kind, id, owner_instance_id, name, revision);
            std::uint32_t _track_count = 0;
            archive(_track_count);
            if (_track_count > protocol_maximum_track_count) {
                throw protocol_error("Catalog contains too many tracks");
            }
            tracks.resize(_track_count);
            for (wire_track& _track : tracks) {
                archive(_track);
            }
        }
    };

    struct wire_endpoint {
        bounded_string host;
        std::uint16_t port { 0 };
        peer_endpoint_family family { peer_endpoint_family::ipv4 };

        template <typename Archive>
        void serialize(Archive& archive)
        {
            archive(host, port, family);
        }
    };

    template <message_kind Kind>
    struct peer_message {
        std::uint32_t magic { message_magic };
        std::uint32_t protocol_version { protocol_current_version };
        message_kind kind { Kind };
        bounded_string id;
        bounded_string name;
        bounded_string token;
        bounded_string fingerprint;
        std::vector<wire_endpoint> endpoints;

        template <typename Archive>
        void save(Archive& archive) const
        {
            archive(magic, protocol_version, kind, id, name, token, fingerprint);
            const std::uint32_t _endpoint_count = static_cast<std::uint32_t>(endpoints.size());
            archive(_endpoint_count);
            for (const wire_endpoint& _endpoint : endpoints) {
                archive(_endpoint);
            }
        }

        template <typename Archive>
        void load(Archive& archive)
        {
            archive(magic, protocol_version, kind, id, name, token, fingerprint);
            std::uint32_t _endpoint_count = 0;
            archive(_endpoint_count);
            if (_endpoint_count > protocol_maximum_endpoint_count) {
                throw protocol_error("Peer message contains too many endpoints");
            }
            endpoints.resize(_endpoint_count);
            for (wire_endpoint& _endpoint : endpoints) {
                archive(_endpoint);
            }
        }
    };

    using discovery_message = peer_message<message_kind::discovery>;
    using invite_message = peer_message<message_kind::invite>;

    constexpr std::string_view invite_prefix = "soundstep:";
    constexpr std::string_view base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    [[nodiscard]] std::string base64url_encode(std::string_view bytes)
    {
        std::string _encoded;
        _encoded.reserve((bytes.size() * 4 + 2) / 3);
        std::uint32_t _buffer = 0;
        int _bits = 0;
        for (const unsigned char _byte : bytes) {
            _buffer = (_buffer << 8) | _byte;
            _bits += 8;
            while (_bits >= 6) {
                _bits -= 6;
                _encoded.push_back(base64_alphabet[(_buffer >> _bits) & 0x3fu]);
            }
        }
        if (_bits != 0) {
            _encoded.push_back(base64_alphabet[(_buffer << (6 - _bits)) & 0x3fu]);
        }
        return _encoded;
    }

    [[nodiscard]] int base64_value(char character)
    {
        const std::size_t _position = base64_alphabet.find(character);
        return _position == std::string_view::npos ? -1 : static_cast<int>(_position);
    }

    [[nodiscard]] std::string base64url_decode(std::string_view encoded)
    {
        if (encoded.empty() || encoded.find('=') != std::string_view::npos || encoded.size() % 4 == 1) {
            throw protocol_error("Pairing code has invalid Base64URL data");
        }

        std::string _decoded;
        _decoded.reserve(encoded.size() * 3 / 4);
        std::uint32_t _buffer = 0;
        int _bits = 0;
        for (const char _character : encoded) {
            const int _value = base64_value(_character);
            if (_value < 0) {
                throw protocol_error("Pairing code has invalid Base64URL data");
            }
            _buffer = (_buffer << 6) | static_cast<std::uint32_t>(_value);
            _bits += 6;
            if (_bits >= 8) {
                _bits -= 8;
                _decoded.push_back(static_cast<char>((_buffer >> _bits) & 0xffu));
            }
        }
        return _decoded;
    }

    void validate_header(std::uint32_t magic, std::uint32_t version, message_kind actual_kind, message_kind expected_kind)
    {
        if (magic != message_magic) {
            throw protocol_error("Invalid Soundstep protocol message");
        }
        if (version != protocol_current_version) {
            throw protocol_error("Unsupported Soundstep protocol version");
        }
        if (actual_kind != expected_kind) {
            throw protocol_error("Unexpected Soundstep protocol message type");
        }
    }

    void validate_hash(std::string_view hash)
    {
        if (hash.size() != 64) {
            throw protocol_error("Track hash must contain 64 hexadecimal characters");
        }
        for (const char _character : hash) {
            const bool _digit = _character >= '0' && _character <= '9';
            const bool _lower = _character >= 'a' && _character <= 'f';
            const bool _upper = _character >= 'A' && _character <= 'F';
            if (!_digit && !_lower && !_upper) {
                throw protocol_error("Track hash must contain 64 hexadecimal characters");
            }
        }
    }

    void validate_fingerprint(std::string_view fingerprint)
    {
        if (fingerprint.size() != 64) {
            throw protocol_error("Transport fingerprint must contain 64 hexadecimal characters");
        }
        for (const char _character : fingerprint) {
            const bool _digit = _character >= '0' && _character <= '9';
            const bool _lower = _character >= 'a' && _character <= 'f';
            const bool _upper = _character >= 'A' && _character <= 'F';
            if (!_digit && !_lower && !_upper) {
                throw protocol_error("Transport fingerprint must contain 64 hexadecimal characters");
            }
        }
    }

    void validate_track(const track& track)
    {
        if (track.id.empty()) {
            throw protocol_error("Track ID cannot be empty");
        }
        validate_hash(track.file_hash);
        if (track.cover_hash.empty()) {
            if (!track.cover_content_type.empty() || track.cover_size_bytes != 0) {
                throw protocol_error("Track cover fields are incomplete");
            }
        } else {
            validate_hash(track.cover_hash);
            if ((track.cover_content_type != "image/jpeg" && track.cover_content_type != "image/png") || track.cover_size_bytes == 0 || track.cover_size_bytes > 8 * 1024 * 1024) {
                throw protocol_error("Track cover fields are invalid");
            }
        }
        if (audio_extension_name(track.extension).empty()) {
            throw protocol_error("Track audio extension is unsupported");
        }
    }

    void validate_endpoint(const peer_endpoint& value)
    {
        if (value.host.empty()) {
            throw protocol_error("Peer endpoint host cannot be empty");
        }
        if (value.port == 0) {
            throw protocol_error("Peer endpoint port cannot be zero");
        }
        if (value.family != peer_endpoint_family::ipv4 && value.family != peer_endpoint_family::ipv6) {
            throw protocol_error("Peer endpoint family is invalid");
        }
    }

    template <typename Message>
    std::string encode(Message& message)
    {
        try {
            std::ostringstream _output(std::ios::binary | std::ios::out);
            {
                cereal::PortableBinaryOutputArchive _archive(_output, cereal::PortableBinaryOutputArchive::Options::LittleEndian());
                _archive(message);
            }
            std::string _bytes = _output.str();
            if (_bytes.size() > protocol_maximum_payload_size) {
                throw protocol_error("Protocol payload exceeds the size limit");
            }
            return _bytes;
        } catch (const protocol_error&) {
            throw;
        } catch (const cereal::Exception& _error) {
            throw protocol_error(std::string("Could not encode protocol message: ") + _error.what());
        }
    }

    template <typename Message>
    Message decode(std::string_view bytes)
    {
        if (bytes.empty()) {
            throw protocol_error("Protocol payload is empty");
        }
        if (bytes.size() > protocol_maximum_payload_size) {
            throw protocol_error("Protocol payload exceeds the size limit");
        }

        try {
            std::istringstream _input(std::string(bytes), std::ios::binary | std::ios::in);
            Message _message;
            {
                cereal::PortableBinaryInputArchive _archive(_input);
                _archive(_message);
            }
            if (_input.peek() != std::char_traits<char>::eof()) {
                throw protocol_error("Protocol payload contains trailing data");
            }
            return _message;
        } catch (const protocol_error&) {
            throw;
        } catch (const cereal::Exception& _error) {
            throw protocol_error(std::string("Could not decode protocol message: ") + _error.what());
        } catch (const std::bad_alloc&) {
            throw protocol_error("Protocol message requested too much memory");
        }
    }

    [[nodiscard]] wire_track to_wire(const track& track)
    {
        validate_track(track);
        const std::string_view _extension = audio_extension_name(track.extension);
        return {
            bounded_string(track.id),
            bounded_string(track.file_hash),
            bounded_string(track.cover_hash),
            bounded_string(track.cover_content_type),
            track.cover_size_bytes,
            bounded_string(std::string(_extension)),
            bounded_string(track.title),
            bounded_string(track.artist),
            bounded_string(track.album),
            track.track_number,
            track.duration_ms,
            track.size_bytes
        };
    }

    [[nodiscard]] track from_wire(const wire_track& value, const std::string& catalog_id)
    {
        const std::optional<audio_extension> _extension = parse_audio_extension(value.extension._value);
        if (!_extension) {
            throw protocol_error("Track audio extension is missing or unsupported");
        }
        track _result {
            value.id._value,
            catalog_id,
            value.file_hash._value,
            value.cover_hash._value,
            value.cover_content_type._value,
            value.cover_size_bytes,
            *_extension,
            value.title._value,
            value.artist._value,
            value.album._value,
            value.track_number,
            value.duration_ms,
            value.size_bytes
        };
        validate_track(_result);
        return _result;
    }

    [[nodiscard]] wire_endpoint to_wire_endpoint(const peer_endpoint& value)
    {
        validate_endpoint(value);
        return { bounded_string(value.host), value.port, value.family };
    }

    [[nodiscard]] peer_endpoint from_wire_endpoint(const wire_endpoint& value)
    {
        peer_endpoint _result { value.host._value, value.port, value.family, 0, 0 };
        validate_endpoint(_result);
        return _result;
    }

    [[nodiscard]] std::vector<wire_endpoint> to_wire_endpoints(const std::vector<peer_endpoint>& endpoints)
    {
        if (endpoints.empty()) {
            throw protocol_error("At least one peer endpoint is required");
        }
        if (endpoints.size() > protocol_maximum_endpoint_count) {
            throw protocol_error("Too many peer endpoints");
        }

        std::vector<wire_endpoint> _result;
        _result.reserve(endpoints.size());
        for (const peer_endpoint& _endpoint : endpoints) {
            _result.push_back(to_wire_endpoint(_endpoint));
        }
        return _result;
    }

    [[nodiscard]] std::vector<peer_endpoint> from_wire_endpoints(const std::vector<wire_endpoint>& endpoints)
    {
        if (endpoints.empty() || endpoints.size() > protocol_maximum_endpoint_count) {
            throw protocol_error("Peer endpoint list is invalid");
        }

        std::vector<peer_endpoint> _result;
        _result.reserve(endpoints.size());
        for (const wire_endpoint& _endpoint : endpoints) {
            _result.push_back(from_wire_endpoint(_endpoint));
        }
        return _result;
    }

}

std::string protocol_encode_instance(const instance_info& instance)
{
    if (instance.id.empty()) {
        throw protocol_error("Instance ID cannot be empty");
    }
    if (instance.name.empty()) {
        throw protocol_error("Instance name cannot be empty");
    }

    instance_message _message;
    _message.id = bounded_string(instance.id);
    _message.name = bounded_string(instance.name);
    return encode(_message);
}

instance_info protocol_decode_instance(std::string_view bytes)
{
    const instance_message _message = decode<instance_message>(bytes);
    validate_header(_message.magic, _message.protocol_version, _message.kind, message_kind::instance);
    if (_message.id._value.empty() || _message.name._value.empty()) {
        throw protocol_error("Instance message contains empty identity fields");
    }
    return { _message.id._value, _message.name._value };
}

std::string protocol_encode_catalog(const catalog_snapshot& catalog)
{
    if (catalog.id.empty()) {
        throw protocol_error("Catalog ID cannot be empty");
    }
    if (catalog.owner_instance_id.empty()) {
        throw protocol_error("Catalog owner cannot be empty");
    }
    if (catalog.name.empty()) {
        throw protocol_error("Catalog name cannot be empty");
    }
    if (catalog.tracks.size() > protocol_maximum_track_count) {
        throw protocol_error("Catalog contains too many tracks");
    }

    catalog_message _message;
    _message.id = bounded_string(catalog.id);
    _message.owner_instance_id = bounded_string(catalog.owner_instance_id);
    _message.name = bounded_string(catalog.name);
    _message.revision = catalog.revision;
    _message.tracks.reserve(catalog.tracks.size());
    for (const track& _value : catalog.tracks) {
        if (_value.catalog_id != catalog.id) {
            throw protocol_error("Track belongs to a different catalog");
        }
        _message.tracks.push_back(to_wire(_value));
    }
    return encode(_message);
}

catalog_snapshot protocol_decode_catalog(std::string_view bytes)
{
    const catalog_message _message = decode<catalog_message>(bytes);
    validate_header(_message.magic, _message.protocol_version, _message.kind, message_kind::catalog);
    if (_message.id._value.empty() || _message.owner_instance_id._value.empty() || _message.name._value.empty()) {
        throw protocol_error("Catalog message contains empty identity fields");
    }

    catalog_snapshot _result;
    _result.id = _message.id._value;
    _result.owner_instance_id = _message.owner_instance_id._value;
    _result.name = _message.name._value;
    _result.revision = _message.revision;
    _result.tracks.reserve(_message.tracks.size());
    for (const wire_track& _value : _message.tracks) {
        _result.tracks.push_back(from_wire(_value, _result.id));
    }
    return _result;
}

std::string protocol_encode_discovery(const peer_discovery& discovery)
{
    if (discovery.instance.id.empty() || discovery.instance.name.empty()) {
        throw protocol_error("Discovery identity cannot be empty");
    }
    if (discovery.token.empty()) {
        throw protocol_error("Discovery token cannot be empty");
    }
    validate_fingerprint(discovery.fingerprint);

    discovery_message _message;
    _message.id = bounded_string(discovery.instance.id);
    _message.name = bounded_string(discovery.instance.name);
    _message.token = bounded_string(discovery.token);
    _message.fingerprint = bounded_string(discovery.fingerprint);
    _message.endpoints = to_wire_endpoints(discovery.endpoints);
    return encode(_message);
}

peer_discovery protocol_decode_discovery(std::string_view bytes)
{
    const discovery_message _message = decode<discovery_message>(bytes);
    validate_header(_message.magic, _message.protocol_version, _message.kind, message_kind::discovery);
    if (_message.id._value.empty() || _message.name._value.empty() || _message.token._value.empty()) {
        throw protocol_error("Discovery message contains invalid fields");
    }
    validate_fingerprint(_message.fingerprint._value);
    return {
        { _message.id._value, _message.name._value },
        from_wire_endpoints(_message.endpoints),
        _message.token._value,
        _message.fingerprint._value
    };
}

std::string protocol_encode_invite(const peer_invite& invite)
{
    if (invite.instance.id.empty() || invite.instance.name.empty()) {
        throw protocol_error("Pairing invite identity cannot be empty");
    }
    if (invite.token.empty()) {
        throw protocol_error("Pairing invite token cannot be empty");
    }
    validate_fingerprint(invite.fingerprint);

    invite_message _message;
    _message.id = bounded_string(invite.instance.id);
    _message.name = bounded_string(invite.instance.name);
    _message.token = bounded_string(invite.token);
    _message.fingerprint = bounded_string(invite.fingerprint);
    _message.endpoints = to_wire_endpoints(invite.endpoints);
    return std::string(invite_prefix) + base64url_encode(encode(_message));
}

peer_invite protocol_decode_invite(std::string_view code)
{
    while (!code.empty() && (code.front() == ' ' || code.front() == '\t' || code.front() == '\r' || code.front() == '\n')) {
        code.remove_prefix(1);
    }
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t' || code.back() == '\r' || code.back() == '\n')) {
        code.remove_suffix(1);
    }
    if (code.substr(0, invite_prefix.size()) != invite_prefix) {
        throw protocol_error("Pairing code must begin with soundstep:");
    }

    const std::string _bytes = base64url_decode(code.substr(invite_prefix.size()));
    const invite_message _message = decode<invite_message>(_bytes);
    validate_header(_message.magic, _message.protocol_version, _message.kind, message_kind::invite);
    if (_message.id._value.empty() || _message.name._value.empty() || _message.token._value.empty()) {
        throw protocol_error("Pairing invite contains invalid fields");
    }
    validate_fingerprint(_message.fingerprint._value);
    return {
        { _message.id._value, _message.name._value },
        from_wire_endpoints(_message.endpoints),
        _message.token._value,
        _message.fingerprint._value
    };
}

}
