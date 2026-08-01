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

    constexpr std::uint32_t _message_magic = 0x53535450u; // SSTP
    constexpr std::size_t _maximum_string_size = 1024 * 1024;

    enum class _message_kind : std::uint8_t {
        instance = 1,
        catalog = 2,
        discovery = 3,
        invite = 4
    };

    struct _bounded_string {
        _bounded_string() = default;
        explicit _bounded_string(std::string text)
            : _value(std::move(text))
        {
            if (_value.size() > _maximum_string_size) {
                throw protocol_error("Protocol string exceeds the size limit");
            }
        }

        template <class Archive>
        void save(Archive& archive) const
        {
            const std::uint32_t _size = static_cast<std::uint32_t>(_value.size());
            archive(_size);
            if (_size != 0) {
                archive(cereal::binary_data(_value.data(), _size));
            }
        }

        template <class Archive>
        void load(Archive& archive)
        {
            std::uint32_t _size = 0;
            archive(_size);
            if (_size > _maximum_string_size) {
                throw protocol_error("Protocol string exceeds the size limit");
            }
            _value.resize(_size);
            if (_size != 0) {
                archive(cereal::binary_data(_value.data(), _size));
            }
        }

        std::string _value;
    };

    struct _wire_track {
        _bounded_string _id;
        _bounded_string _file_hash;
        _bounded_string _cover_hash;
        _bounded_string _cover_content_type;
        std::uint64_t _cover_size_bytes { 0 };
        _bounded_string _extension;
        _bounded_string _title;
        _bounded_string _artist;
        _bounded_string _album;
        std::uint32_t _track_number { 0 };
        std::uint64_t _duration_ms { 0 };
        std::uint64_t _size_bytes { 0 };

        template <class Archive>
        void serialize(Archive& archive)
        {
            archive(
                _id,
                _file_hash,
                _cover_hash,
                _cover_content_type,
                _cover_size_bytes,
                _extension,
                _title,
                _artist,
                _album,
                _track_number,
                _duration_ms,
                _size_bytes);
        }
    };

    struct _instance_message {
        std::uint32_t _magic { _message_magic };
        std::uint32_t _protocol_version { protocol_current_version };
        _message_kind _kind { _message_kind::instance };
        _bounded_string _id;
        _bounded_string _name;

        template <class Archive>
        void serialize(Archive& archive)
        {
            archive(_magic, _protocol_version, _kind, _id, _name);
        }
    };

    struct _catalog_message {
        std::uint32_t _magic { _message_magic };
        std::uint32_t _protocol_version { protocol_current_version };
        _message_kind _kind { _message_kind::catalog };
        _bounded_string _id;
        _bounded_string _owner_instance_id;
        _bounded_string _name;
        std::uint64_t _revision { 0 };
        std::vector<_wire_track> _tracks;

        template <class Archive>
        void save(Archive& archive) const
        {
            if (_tracks.size() > protocol_maximum_track_count) {
                throw protocol_error("Catalog contains too many tracks");
            }

            archive(_magic, _protocol_version, _kind, _id, _owner_instance_id, _name, _revision);
            const std::uint32_t _track_count = static_cast<std::uint32_t>(_tracks.size());
            archive(_track_count);
            for (const _wire_track& _track : _tracks) {
                archive(_track);
            }
        }

        template <class Archive>
        void load(Archive& archive)
        {
            archive(_magic, _protocol_version, _kind, _id, _owner_instance_id, _name, _revision);
            std::uint32_t _track_count = 0;
            archive(_track_count);
            if (_track_count > protocol_maximum_track_count) {
                throw protocol_error("Catalog contains too many tracks");
            }
            _tracks.resize(_track_count);
            for (_wire_track& _track : _tracks) {
                archive(_track);
            }
        }
    };

    struct _wire_endpoint {
        _bounded_string _host;
        std::uint16_t _port { 0 };
        peer_endpoint_family _family { peer_endpoint_family::ipv4 };

        template <class Archive>
        void serialize(Archive& archive)
        {
            archive(_host, _port, _family);
        }
    };

    template <_message_kind Kind>
    struct _peer_message {
        std::uint32_t _magic { _message_magic };
        std::uint32_t _protocol_version { protocol_current_version };
        _message_kind _kind { Kind };
        _bounded_string _id;
        _bounded_string _name;
        _bounded_string _token;
        _bounded_string _fingerprint;
        std::vector<_wire_endpoint> _endpoints;

        template <class Archive>
        void save(Archive& archive) const
        {
            archive(_magic, _protocol_version, _kind, _id, _name, _token, _fingerprint);
            const std::uint32_t _endpoint_count = static_cast<std::uint32_t>(_endpoints.size());
            archive(_endpoint_count);
            for (const _wire_endpoint& _endpoint : _endpoints) {
                archive(_endpoint);
            }
        }

        template <class Archive>
        void load(Archive& archive)
        {
            archive(_magic, _protocol_version, _kind, _id, _name, _token, _fingerprint);
            std::uint32_t _endpoint_count = 0;
            archive(_endpoint_count);
            if (_endpoint_count > protocol_maximum_endpoint_count) {
                throw protocol_error("Peer message contains too many endpoints");
            }
            _endpoints.resize(_endpoint_count);
            for (_wire_endpoint& _endpoint : _endpoints) {
                archive(_endpoint);
            }
        }
    };

    using _discovery_message = _peer_message<_message_kind::discovery>;
    using _invite_message = _peer_message<_message_kind::invite>;

    constexpr std::string_view _invite_prefix = "soundstep:";
    constexpr std::string_view _base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string _base64url_encode(std::string_view bytes)
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
                _encoded.push_back(_base64_alphabet[(_buffer >> _bits) & 0x3fu]);
            }
        }
        if (_bits != 0) {
            _encoded.push_back(_base64_alphabet[(_buffer << (6 - _bits)) & 0x3fu]);
        }
        return _encoded;
    }

    int _base64_value(char character)
    {
        const std::size_t _position = _base64_alphabet.find(character);
        return _position == std::string_view::npos ? -1 : static_cast<int>(_position);
    }

    std::string _base64url_decode(std::string_view encoded)
    {
        if (encoded.empty() || encoded.find('=') != std::string_view::npos || encoded.size() % 4 == 1) {
            throw protocol_error("Pairing code has invalid Base64URL data");
        }

        std::string _decoded;
        _decoded.reserve(encoded.size() * 3 / 4);
        std::uint32_t _buffer = 0;
        int _bits = 0;
        for (const char _character : encoded) {
            const int _value = _base64_value(_character);
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

    void _validate_header(
        std::uint32_t magic,
        std::uint32_t version,
        _message_kind actual_kind,
        _message_kind expected_kind)
    {
        if (magic != _message_magic) {
            throw protocol_error("Invalid Soundstep protocol message");
        }
        if (version != protocol_current_version) {
            throw protocol_error("Unsupported Soundstep protocol version");
        }
        if (actual_kind != expected_kind) {
            throw protocol_error("Unexpected Soundstep protocol message type");
        }
    }

    void _validate_hash(std::string_view hash)
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

    void _validate_fingerprint(std::string_view fingerprint)
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

    void _validate_track(const track& track)
    {
        if (track.id.empty()) {
            throw protocol_error("Track ID cannot be empty");
        }
        _validate_hash(track.file_hash);
        if (track.cover_hash.empty()) {
            if (!track.cover_content_type.empty() || track.cover_size_bytes != 0) {
                throw protocol_error("Track cover fields are incomplete");
            }
        } else {
            _validate_hash(track.cover_hash);
            if ((track.cover_content_type != "image/jpeg"
                    && track.cover_content_type != "image/png")
                || track.cover_size_bytes == 0
                || track.cover_size_bytes > 8 * 1024 * 1024) {
                throw protocol_error("Track cover fields are invalid");
            }
        }
        if (audio_extension_name(track.extension).empty()) {
            throw protocol_error("Track audio extension is unsupported");
        }
    }

    void _validate_endpoint(const peer_endpoint& value)
    {
        if (value.host.empty()) {
            throw protocol_error("Peer endpoint host cannot be empty");
        }
        if (value.port == 0) {
            throw protocol_error("Peer endpoint port cannot be zero");
        }
        if (value.family != peer_endpoint_family::ipv4
            && value.family != peer_endpoint_family::ipv6) {
            throw protocol_error("Peer endpoint family is invalid");
        }
    }

    template <class Message>
    std::string _encode(Message& message)
    {
        try {
            std::ostringstream _output(std::ios::binary | std::ios::out);
            {
                cereal::PortableBinaryOutputArchive _archive(
                    _output,
                    cereal::PortableBinaryOutputArchive::Options::LittleEndian());
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

    template <class Message>
    Message _decode(std::string_view bytes)
    {
        if (bytes.empty()) {
            throw protocol_error("Protocol payload is empty");
        }
        if (bytes.size() > protocol_maximum_payload_size) {
            throw protocol_error("Protocol payload exceeds the size limit");
        }

        try {
            std::istringstream _input(
                std::string(bytes),
                std::ios::binary | std::ios::in);
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

    _wire_track _to_wire(const track& track)
    {
        _validate_track(track);
        const std::string_view _extension = audio_extension_name(track.extension);
        return {
            _bounded_string(track.id),
            _bounded_string(track.file_hash),
            _bounded_string(track.cover_hash),
            _bounded_string(track.cover_content_type),
            track.cover_size_bytes,
            _bounded_string(std::string(_extension)),
            _bounded_string(track.title),
            _bounded_string(track.artist),
            _bounded_string(track.album),
            track.track_number,
            track.duration_ms,
            track.size_bytes
        };
    }

    track _from_wire(const _wire_track& value, const std::string& catalog_id)
    {
        const std::optional<audio_extension> _extension = parse_audio_extension(value._extension._value);
        if (!_extension) {
            throw protocol_error("Track audio extension is missing or unsupported");
        }
        track _result {
            value._id._value,
            catalog_id,
            value._file_hash._value,
            value._cover_hash._value,
            value._cover_content_type._value,
            value._cover_size_bytes,
            *_extension,
            value._title._value,
            value._artist._value,
            value._album._value,
            value._track_number,
            value._duration_ms,
            value._size_bytes
        };
        _validate_track(_result);
        return _result;
    }

    _wire_endpoint _to_wire_endpoint(const peer_endpoint& value)
    {
        _validate_endpoint(value);
        return { _bounded_string(value.host), value.port, value.family };
    }

    peer_endpoint _from_wire_endpoint(const _wire_endpoint& value)
    {
        peer_endpoint _result { value._host._value, value._port, value._family, 0, 0 };
        _validate_endpoint(_result);
        return _result;
    }

    std::vector<_wire_endpoint> _to_wire_endpoints(const std::vector<peer_endpoint>& endpoints)
    {
        if (endpoints.empty()) {
            throw protocol_error("At least one peer endpoint is required");
        }
        if (endpoints.size() > protocol_maximum_endpoint_count) {
            throw protocol_error("Too many peer endpoints");
        }

        std::vector<_wire_endpoint> _result;
        _result.reserve(endpoints.size());
        for (const peer_endpoint& _endpoint : endpoints) {
            _result.push_back(_to_wire_endpoint(_endpoint));
        }
        return _result;
    }

    std::vector<peer_endpoint> _from_wire_endpoints(const std::vector<_wire_endpoint>& endpoints)
    {
        if (endpoints.empty() || endpoints.size() > protocol_maximum_endpoint_count) {
            throw protocol_error("Peer endpoint list is invalid");
        }

        std::vector<peer_endpoint> _result;
        _result.reserve(endpoints.size());
        for (const _wire_endpoint& _endpoint : endpoints) {
            _result.push_back(_from_wire_endpoint(_endpoint));
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

    _instance_message _message;
    _message._id = _bounded_string(instance.id);
    _message._name = _bounded_string(instance.name);
    return _encode(_message);
}

instance_info protocol_decode_instance(std::string_view bytes)
{
    const _instance_message _message = _decode<_instance_message>(bytes);
    _validate_header(
        _message._magic,
        _message._protocol_version,
        _message._kind,
        _message_kind::instance);
    if (_message._id._value.empty() || _message._name._value.empty()) {
        throw protocol_error("Instance message contains empty identity fields");
    }
    return { _message._id._value, _message._name._value };
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

    _catalog_message _message;
    _message._id = _bounded_string(catalog.id);
    _message._owner_instance_id = _bounded_string(catalog.owner_instance_id);
    _message._name = _bounded_string(catalog.name);
    _message._revision = catalog.revision;
    _message._tracks.reserve(catalog.tracks.size());
    for (const track& _value : catalog.tracks) {
        if (_value.catalog_id != catalog.id) {
            throw protocol_error("Track belongs to a different catalog");
        }
        _message._tracks.push_back(_to_wire(_value));
    }
    return _encode(_message);
}

catalog_snapshot protocol_decode_catalog(std::string_view bytes)
{
    const _catalog_message _message = _decode<_catalog_message>(bytes);
    _validate_header(
        _message._magic,
        _message._protocol_version,
        _message._kind,
        _message_kind::catalog);
    if (_message._id._value.empty() || _message._owner_instance_id._value.empty() || _message._name._value.empty()) {
        throw protocol_error("Catalog message contains empty identity fields");
    }

    catalog_snapshot _result;
    _result.id = _message._id._value;
    _result.owner_instance_id = _message._owner_instance_id._value;
    _result.name = _message._name._value;
    _result.revision = _message._revision;
    _result.tracks.reserve(_message._tracks.size());
    for (const _wire_track& _value : _message._tracks) {
        _result.tracks.push_back(_from_wire(_value, _result.id));
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
    _validate_fingerprint(discovery.fingerprint);

    _discovery_message _message;
    _message._id = _bounded_string(discovery.instance.id);
    _message._name = _bounded_string(discovery.instance.name);
    _message._token = _bounded_string(discovery.token);
    _message._fingerprint = _bounded_string(discovery.fingerprint);
    _message._endpoints = _to_wire_endpoints(discovery.endpoints);
    return _encode(_message);
}

peer_discovery protocol_decode_discovery(std::string_view bytes)
{
    const _discovery_message _message = _decode<_discovery_message>(bytes);
    _validate_header(
        _message._magic,
        _message._protocol_version,
        _message._kind,
        _message_kind::discovery);
    if (_message._id._value.empty() || _message._name._value.empty() || _message._token._value.empty()) {
        throw protocol_error("Discovery message contains invalid fields");
    }
    _validate_fingerprint(_message._fingerprint._value);
    return {
        { _message._id._value, _message._name._value },
        _from_wire_endpoints(_message._endpoints),
        _message._token._value,
        _message._fingerprint._value
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
    _validate_fingerprint(invite.fingerprint);

    _invite_message _message;
    _message._id = _bounded_string(invite.instance.id);
    _message._name = _bounded_string(invite.instance.name);
    _message._token = _bounded_string(invite.token);
    _message._fingerprint = _bounded_string(invite.fingerprint);
    _message._endpoints = _to_wire_endpoints(invite.endpoints);
    return std::string(_invite_prefix) + _base64url_encode(_encode(_message));
}

peer_invite protocol_decode_invite(std::string_view code)
{
    while (!code.empty() && (code.front() == ' ' || code.front() == '\t' || code.front() == '\r' || code.front() == '\n')) {
        code.remove_prefix(1);
    }
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t' || code.back() == '\r' || code.back() == '\n')) {
        code.remove_suffix(1);
    }
    if (code.substr(0, _invite_prefix.size()) != _invite_prefix) {
        throw protocol_error("Pairing code must begin with soundstep:");
    }

    const std::string _bytes = _base64url_decode(code.substr(_invite_prefix.size()));
    const _invite_message _message = _decode<_invite_message>(_bytes);
    _validate_header(_message._magic, _message._protocol_version, _message._kind, _message_kind::invite);
    if (_message._id._value.empty() || _message._name._value.empty() || _message._token._value.empty()) {
        throw protocol_error("Pairing invite contains invalid fields");
    }
    _validate_fingerprint(_message._fingerprint._value);
    return {
        { _message._id._value, _message._name._value },
        _from_wire_endpoints(_message._endpoints),
        _message._token._value,
        _message._fingerprint._value
    };
}

}
