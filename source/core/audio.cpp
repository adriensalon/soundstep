#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include <miniaudio.h>

#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

#include <core/audio.hpp>
#include <core/peer.hpp>

namespace soundstep {
namespace {

    constexpr std::size_t _maximum_cover_size = 8 * 1024 * 1024;

    bool _is_audio_asset_hash(std::string_view hash)
    {
        if (hash.size() != 64) {
            return false;
        }
        for (const char _character : hash) {
            const bool _digit = _character >= '0' && _character <= '9';
            const bool _lower = _character >= 'a' && _character <= 'f';
            const bool _upper = _character >= 'A' && _character <= 'F';
            if (!_digit && !_lower && !_upper) {
                return false;
            }
        }
        return true;
    }

    void _append_utf8(std::string& output, std::uint32_t codepoint)
    {
        if (codepoint <= 0x7f) {
            output += static_cast<char>(codepoint);
        } else if (codepoint <= 0x7ff) {
            output += static_cast<char>(0xc0 | (codepoint >> 6));
            output += static_cast<char>(0x80 | (codepoint & 0x3f));
        } else if (codepoint <= 0xffff) {
            output += static_cast<char>(0xe0 | (codepoint >> 12));
            output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (codepoint & 0x3f));
        } else if (codepoint <= 0x10ffff) {
            output += static_cast<char>(0xf0 | (codepoint >> 18));
            output += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
            output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (codepoint & 0x3f));
        }
    }

    std::string _clean_text(std::string value)
    {
        const std::string::size_type _null = value.find('\0');
        if (_null != std::string::npos) {
            value.resize(_null);
        }
        while (!value.empty() && static_cast<unsigned char>(value.back()) <= 0x20) {
            value.pop_back();
        }
        std::size_t _first = 0;
        while (_first < value.size() && static_cast<unsigned char>(value[_first]) <= 0x20) {
            ++_first;
        }
        value.erase(0, _first);
        return value;
    }

    std::string _latin1_to_utf8(const unsigned char* data, std::size_t size)
    {
        std::string _result;
        _result.reserve(size);
        for (std::size_t _index = 0; _index < size && data[_index] != 0; ++_index) {
            _append_utf8(_result, data[_index]);
        }
        return _clean_text(std::move(_result));
    }

    std::uint16_t _utf16_unit(const unsigned char* value, bool big_endian)
    {
        return static_cast<std::uint16_t>(big_endian
                ? (static_cast<unsigned>(value[0]) << 8) | value[1]
                : (static_cast<unsigned>(value[1]) << 8) | value[0]);
    }

    std::string _utf16_to_utf8(const unsigned char* data, std::size_t size, bool big_endian)
    {
        if (size >= 2 && data[0] == 0xff && data[1] == 0xfe) {
            big_endian = false;
            data += 2;
            size -= 2;
        } else if (size >= 2 && data[0] == 0xfe && data[1] == 0xff) {
            big_endian = true;
            data += 2;
            size -= 2;
        }

        std::string _result;
        for (std::size_t _index = 0; _index + 1 < size; _index += 2) {
            std::uint32_t _codepoint = _utf16_unit(data + _index, big_endian);
            if (_codepoint == 0) {
                break;
            }
            if (_codepoint >= 0xd800 && _codepoint <= 0xdbff && _index + 3 < size) {
                const std::uint32_t _low = _utf16_unit(data + _index + 2, big_endian);
                if (_low >= 0xdc00 && _low <= 0xdfff) {
                    _codepoint = 0x10000 + ((_codepoint - 0xd800) << 10) + (_low - 0xdc00);
                    _index += 2;
                }
            }
            _append_utf8(_result, _codepoint);
        }
        return _clean_text(std::move(_result));
    }

    std::string _id3_text(const unsigned char* data, std::size_t size)
    {
        if (size <= 1) {
            return { };
        }
        switch (data[0]) {
        case 0:
            return _latin1_to_utf8(data + 1, size - 1);
        case 1:
            return _utf16_to_utf8(data + 1, size - 1, false);
        case 2:
            return _utf16_to_utf8(data + 1, size - 1, true);
        case 3:
            return _clean_text(std::string(reinterpret_cast<const char*>(data + 1), size - 1));
        default:
            return { };
        }
    }

    std::uint32_t _parse_track_number(std::string_view value)
    {
        std::uint64_t _result = 0;
        bool _found_digit = false;
        for (const char _character : value) {
            if (_character >= '0' && _character <= '9') {
                _found_digit = true;
                _result = _result * 10 + static_cast<unsigned>(_character - '0');
                if (_result > (std::numeric_limits<std::uint32_t>::max)()) {
                    return 0;
                }
            } else if (_found_digit) {
                break;
            } else if (_character != ' ' && _character != '\t') {
                return 0;
            }
        }
        return _found_digit ? static_cast<std::uint32_t>(_result) : 0;
    }

    std::uint32_t _big_endian_u32(const unsigned char* data)
    {
        return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) | (static_cast<std::uint32_t>(data[2]) << 8) | data[3];
    }

    std::uint32_t _synchsafe_u32(const unsigned char* data)
    {
        return (static_cast<std::uint32_t>(data[0] & 0x7f) << 21) | (static_cast<std::uint32_t>(data[1] & 0x7f) << 14) | (static_cast<std::uint32_t>(data[2] & 0x7f) << 7) | (data[3] & 0x7f);
    }

    void _set_cover(audio_metadata& metadata, const unsigned char* data, std::size_t size)
    {
        if (!metadata.cover_bytes.empty() || size == 0 || size > _maximum_cover_size) {
            return;
        }

        std::string _content_type;
        if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) {
            _content_type = "image/jpeg";
        } else if (size >= 8
            && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
            _content_type = "image/png";
        } else {
            return;
        }

        metadata.cover_content_type = std::move(_content_type);
        metadata.cover_bytes.assign(data, data + size);
    }

    std::size_t _id3_terminated_size(const unsigned char* data, std::size_t size, unsigned encoding)
    {
        if (encoding == 1 || encoding == 2) {
            for (std::size_t _index = 0; _index + 1 < size; _index += 2) {
                if (data[_index] == 0 && data[_index + 1] == 0) {
                    return _index + 2;
                }
            }
            return size;
        }
        const void* _terminator = std::memchr(data, 0, size);
        return _terminator == nullptr
            ? size
            : static_cast<const unsigned char*>(_terminator) - data + 1;
    }

    void _apply_id3_cover(audio_metadata& metadata, std::string_view id, const unsigned char* data, std::size_t size)
    {
        if (!metadata.cover_bytes.empty() || size < 5 || (id != "APIC" && id != "PIC")) {
            return;
        }

        const unsigned _encoding = data[0];
        std::size_t _cursor = 1;
        if (id == "APIC") {
            const void* _mime_end = std::memchr(data + _cursor, 0, size - _cursor);
            if (_mime_end == nullptr) {
                return;
            }
            _cursor = static_cast<const unsigned char*>(_mime_end) - data + 1;
        } else {
            _cursor += 3;
        }
        if (_cursor >= size) {
            return;
        }
        ++_cursor;
        if (_cursor >= size) {
            return;
        }
        _cursor += _id3_terminated_size(data + _cursor, size - _cursor, _encoding);
        if (_cursor < size) {
            _set_cover(metadata, data + _cursor, size - _cursor);
        }
    }

    void _apply_id3_frame(audio_metadata& metadata, std::string_view id, const unsigned char* data, std::size_t size)
    {
        _apply_id3_cover(metadata, id, data, size);
        const std::string _value = _id3_text(data, size);
        if (_value.empty()) {
            return;
        }
        if (id == "TIT2" || id == "TT2") {
            metadata.title = _value;
        } else if (id == "TPE1" || id == "TP1") {
            metadata.artist = _value;
        } else if (id == "TALB" || id == "TAL") {
            metadata.album = _value;
        } else if (id == "TRCK" || id == "TRK") {
            metadata.track_number = _parse_track_number(_value);
        }
    }

    void _parse_id3v2(audio_metadata& metadata, const unsigned char* raw, std::size_t raw_size)
    {
        if (raw_size < 10 || std::memcmp(raw, "ID3", 3) != 0) {
            return;
        }
        const unsigned _version = raw[3];
        if (_version < 2 || _version > 4) {
            return;
        }

        const std::size_t _declared_size = _synchsafe_u32(raw + 6);
        const std::size_t _body_size = (std::min)(_declared_size, raw_size - 10);
        std::vector<unsigned char> _body(raw + 10, raw + 10 + _body_size);
        if ((raw[5] & 0x80) != 0) {
            std::vector<unsigned char> _decoded;
            _decoded.reserve(_body.size());
            for (std::size_t _index = 0; _index < _body.size(); ++_index) {
                _decoded.push_back(_body[_index]);
                if (_body[_index] == 0xff && _index + 1 < _body.size() && _body[_index + 1] == 0) {
                    ++_index;
                }
            }
            _body = std::move(_decoded);
        }

        std::size_t _cursor = 0;
        if ((raw[5] & 0x40) != 0 && _version >= 3 && _body.size() >= 4) {
            const std::size_t _extended = _version == 4
                ? _synchsafe_u32(_body.data())
                : 4 + _big_endian_u32(_body.data());
            if (_extended > _body.size()) {
                return;
            }
            _cursor = _extended;
        }

        while (_cursor < _body.size()) {
            const std::size_t _header_size = _version == 2 ? 6 : 10;
            if (_body.size() - _cursor < _header_size || _body[_cursor] == 0) {
                break;
            }

            const std::size_t _id_size = _version == 2 ? 3 : 4;
            const std::string_view _id(
                reinterpret_cast<const char*>(_body.data() + _cursor),
                _id_size);
            const std::size_t _frame_size = _version == 2
                ? (static_cast<std::size_t>(_body[_cursor + 3]) << 16) | (static_cast<std::size_t>(_body[_cursor + 4]) << 8) | _body[_cursor + 5]
                : _version == 4
                ? _synchsafe_u32(_body.data() + _cursor + 4)
                : _big_endian_u32(_body.data() + _cursor + 4);
            _cursor += _header_size;
            if (_frame_size == 0 || _frame_size > _body.size() - _cursor) {
                break;
            }

            _apply_id3_frame(metadata, _id, _body.data() + _cursor, _frame_size);
            _cursor += _frame_size;
        }
    }

    void _parse_id3v1(audio_metadata& metadata, const unsigned char* raw, std::size_t raw_size)
    {
        if (raw_size < 128 || std::memcmp(raw, "TAG", 3) != 0) {
            return;
        }
        metadata.title = _latin1_to_utf8(raw + 3, 30);
        metadata.artist = _latin1_to_utf8(raw + 33, 30);
        metadata.album = _latin1_to_utf8(raw + 63, 30);
    }

    bool _equal_ascii(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t _index = 0; _index < left.size(); ++_index) {
            if (std::tolower(static_cast<unsigned char>(left[_index])) != std::tolower(static_cast<unsigned char>(right[_index]))) {
                return false;
            }
        }
        return true;
    }

    int _base64_value(char character)
    {
        if (character >= 'A' && character <= 'Z') {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z') {
            return character - 'a' + 26;
        }
        if (character >= '0' && character <= '9') {
            return character - '0' + 52;
        }
        if (character == '+') {
            return 62;
        }
        if (character == '/') {
            return 63;
        }
        return -1;
    }

    std::vector<unsigned char> _base64_decode(std::string_view encoded)
    {
        std::vector<unsigned char> _result;
        if (encoded.size() > (_maximum_cover_size * 4 + 2) / 3 + 4) {
            return _result;
        }
        std::uint32_t _buffer = 0;
        int _bits = 0;
        for (const char _character : encoded) {
            if (_character == '=') {
                break;
            }
            const int _value = _base64_value(_character);
            if (_value < 0) {
                return { };
            }
            _buffer = (_buffer << 6) | static_cast<std::uint32_t>(_value);
            _bits += 6;
            if (_bits >= 8) {
                _bits -= 8;
                _result.push_back(static_cast<unsigned char>((_buffer >> _bits) & 0xff));
                if (_result.size() > _maximum_cover_size) {
                    return { };
                }
            }
        }
        return _result;
    }

    void _parse_flac_picture(audio_metadata& metadata, const unsigned char* data, std::size_t size)
    {
        if (!metadata.cover_bytes.empty() || size < 32) {
            return;
        }
        std::size_t _cursor = 4;
        const std::uint32_t _mime_size = _big_endian_u32(data + _cursor);
        _cursor += 4;
        if (_mime_size > size - _cursor) {
            return;
        }
        _cursor += _mime_size;
        if (size - _cursor < 4) {
            return;
        }
        const std::uint32_t _description_size = _big_endian_u32(data + _cursor);
        _cursor += 4;
        if (_description_size > size - _cursor) {
            return;
        }
        _cursor += _description_size;
        if (size - _cursor < 20) {
            return;
        }
        _cursor += 16;
        const std::uint32_t _picture_size = _big_endian_u32(data + _cursor);
        _cursor += 4;
        if (_picture_size <= size - _cursor) {
            _set_cover(metadata, data + _cursor, _picture_size);
        }
    }

    void _parse_vorbis_comment(audio_metadata& metadata, std::string_view comment)
    {
        const std::string_view::size_type _separator = comment.find('=');
        if (_separator == std::string_view::npos) {
            return;
        }
        const std::string_view _key = comment.substr(0, _separator);
        const std::string _value = _clean_text(std::string(comment.substr(_separator + 1)));
        if (_value.empty()) {
            return;
        }
        if (_equal_ascii(_key, "TITLE") && metadata.title.empty()) {
            metadata.title = _value;
        } else if (_equal_ascii(_key, "ARTIST") && metadata.artist.empty()) {
            metadata.artist = _value;
        } else if (_equal_ascii(_key, "ALBUM") && metadata.album.empty()) {
            metadata.album = _value;
        } else if (_equal_ascii(_key, "TRACKNUMBER") && metadata.track_number == 0) {
            metadata.track_number = _parse_track_number(_value);
        } else if (_equal_ascii(_key, "METADATA_BLOCK_PICTURE") && metadata.cover_bytes.empty()) {
            const std::vector<unsigned char> _picture = _base64_decode(_value);
            if (!_picture.empty()) {
                _parse_flac_picture(metadata, _picture.data(), _picture.size());
            }
        }
    }

    std::uint64_t _duration_milliseconds(std::uint64_t frames, std::uint32_t sample_rate)
    {
        if (sample_rate == 0) {
            return 0;
        }
        return (frames / sample_rate) * 1'000 + ((frames % sample_rate) * 1'000) / sample_rate;
    }

    std::uint32_t _little_endian_u32(const unsigned char* data)
    {
        return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) | (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
    }

    audio_metadata _inspect_mp3_tags(const std::filesystem::path& path)
    {
        audio_metadata _metadata;
        std::ifstream _input(path, std::ios::binary);
        if (!_input) {
            throw audio_error("Could not open MP3 file: " + path.u8string());
        }

        _input.seekg(0, std::ios::end);
        const std::streamoff _length = _input.tellg();
        if (_length >= 128) {
            std::array<unsigned char, 128> _id3v1 { };
            _input.seekg(_length - static_cast<std::streamoff>(_id3v1.size()), std::ios::beg);
            _input.read(reinterpret_cast<char*>(_id3v1.data()), _id3v1.size());
            if (_input) {
                _parse_id3v1(_metadata, _id3v1.data(), _id3v1.size());
            }
        }

        _input.clear();
        _input.seekg(0, std::ios::beg);
        std::array<unsigned char, 10> _header { };
        _input.read(reinterpret_cast<char*>(_header.data()), _header.size());
        if (_input && std::memcmp(_header.data(), "ID3", 3) == 0) {
            constexpr std::size_t _maximum_tag_size = 16 * 1024 * 1024;
            const std::size_t _body_size = _synchsafe_u32(_header.data() + 6);
            if (_body_size <= _maximum_tag_size && _length >= 0 && _body_size <= static_cast<std::uint64_t>(_length) - _header.size()) {
                std::vector<unsigned char> _tag(_header.begin(), _header.end());
                _tag.resize(_header.size() + _body_size);
                _input.read(
                    reinterpret_cast<char*>(_tag.data() + _header.size()),
                    static_cast<std::streamsize>(_body_size));
                if (_input) {
                    _parse_id3v2(_metadata, _tag.data(), _tag.size());
                }
            }
        }
        return _metadata;
    }

    audio_metadata _inspect_flac_tags(const std::filesystem::path& path)
    {
        audio_metadata _metadata;
        std::ifstream _input(path, std::ios::binary);
        std::array<unsigned char, 4> _signature { };
        _input.read(reinterpret_cast<char*>(_signature.data()), _signature.size());
        if (!_input || std::memcmp(_signature.data(), "fLaC", 4) != 0) {
            throw audio_error("Could not inspect FLAC file: " + path.u8string());
        }

        bool _last = false;
        while (!_last && _input) {
            std::array<unsigned char, 4> _header { };
            _input.read(reinterpret_cast<char*>(_header.data()), _header.size());
            if (!_input) {
                break;
            }
            _last = (_header[0] & 0x80) != 0;
            const unsigned _type = _header[0] & 0x7f;
            const std::size_t _size = (static_cast<std::size_t>(_header[1]) << 16) | (static_cast<std::size_t>(_header[2]) << 8) | _header[3];
            if (_type == 6 && _size <= _maximum_cover_size) {
                std::vector<unsigned char> _block(_size);
                _input.read(reinterpret_cast<char*>(_block.data()), static_cast<std::streamsize>(_block.size()));
                if (_input) {
                    _parse_flac_picture(_metadata, _block.data(), _block.size());
                }
                continue;
            }
            if (_type != 4) {
                _input.seekg(static_cast<std::streamoff>(_size), std::ios::cur);
                continue;
            }

            std::vector<unsigned char> _block(_size);
            _input.read(reinterpret_cast<char*>(_block.data()), static_cast<std::streamsize>(_block.size()));
            if (!_input || _block.size() < 8) {
                break;
            }

            std::size_t _cursor = 0;
            const std::size_t _vendor_size = _little_endian_u32(_block.data());
            _cursor += 4;
            if (_vendor_size > _block.size() - _cursor) {
                break;
            }
            _cursor += _vendor_size;
            if (_block.size() - _cursor < 4) {
                break;
            }
            const std::uint32_t _comment_count = _little_endian_u32(_block.data() + _cursor);
            _cursor += 4;
            for (std::uint32_t _index = 0; _index < _comment_count && _block.size() - _cursor >= 4; ++_index) {
                const std::size_t _comment_size = _little_endian_u32(_block.data() + _cursor);
                _cursor += 4;
                if (_comment_size > _block.size() - _cursor) {
                    break;
                }
                _parse_vorbis_comment(_metadata, std::string_view(reinterpret_cast<const char*>(_block.data() + _cursor), _comment_size));
                _cursor += _comment_size;
            }
        }
        return _metadata;
    }

    audio_metadata _inspect_wav_tags(const std::filesystem::path& path)
    {
        audio_metadata _metadata;
        std::ifstream _input(path, std::ios::binary);
        std::array<unsigned char, 12> _riff { };
        _input.read(reinterpret_cast<char*>(_riff.data()), _riff.size());
        if (!_input || std::memcmp(_riff.data(), "RIFF", 4) != 0 || std::memcmp(_riff.data() + 8, "WAVE", 4) != 0) {
            throw audio_error("Could not inspect WAV file: " + path.u8string());
        }

        while (_input) {
            std::array<unsigned char, 8> _chunk { };
            _input.read(reinterpret_cast<char*>(_chunk.data()), _chunk.size());
            if (!_input) {
                break;
            }
            const std::size_t _chunk_size = _little_endian_u32(_chunk.data() + 4);
            const std::streamoff _next = _input.tellg() + static_cast<std::streamoff>(_chunk_size + (_chunk_size & 1));
            if ((std::memcmp(_chunk.data(), "id3 ", 4) == 0
                    || std::memcmp(_chunk.data(), "ID3 ", 4) == 0)
                && _chunk_size <= _maximum_cover_size) {
                std::vector<unsigned char> _tag(_chunk_size);
                _input.read(reinterpret_cast<char*>(_tag.data()), static_cast<std::streamsize>(_tag.size()));
                if (_input) {
                    _parse_id3v2(_metadata, _tag.data(), _tag.size());
                }
                _input.clear();
                _input.seekg(_next, std::ios::beg);
                continue;
            }
            if (std::memcmp(_chunk.data(), "LIST", 4) != 0 || _chunk_size < 4) {
                _input.seekg(_next, std::ios::beg);
                continue;
            }

            std::array<char, 4> _list_type { };
            _input.read(_list_type.data(), _list_type.size());
            if (!_input || std::memcmp(_list_type.data(), "INFO", 4) != 0) {
                _input.clear();
                _input.seekg(_next, std::ios::beg);
                continue;
            }

            std::size_t _consumed = 4;
            while (_consumed + 8 <= _chunk_size && _input) {
                std::array<unsigned char, 8> _field { };
                _input.read(reinterpret_cast<char*>(_field.data()), _field.size());
                if (!_input) {
                    break;
                }
                const std::size_t _field_size = _little_endian_u32(_field.data() + 4);
                _consumed += 8;
                if (_field_size > _chunk_size - _consumed) {
                    break;
                }
                std::vector<unsigned char> _text(_field_size);
                _input.read(reinterpret_cast<char*>(_text.data()), static_cast<std::streamsize>(_text.size()));
                if (!_input) {
                    break;
                }
                const std::string _value = _latin1_to_utf8(_text.data(), _text.size());
                if (std::memcmp(_field.data(), "INAM", 4) == 0) {
                    _metadata.title = _value;
                } else if (std::memcmp(_field.data(), "IART", 4) == 0) {
                    _metadata.artist = _value;
                } else if (std::memcmp(_field.data(), "IPRD", 4) == 0) {
                    _metadata.album = _value;
                } else if (std::memcmp(_field.data(), "IPRT", 4) == 0) {
                    _metadata.track_number = _parse_track_number(_value);
                }
                _consumed += _field_size;
                if ((_field_size & 1) != 0) {
                    _input.seekg(1, std::ios::cur);
                    ++_consumed;
                }
            }
            _input.clear();
            _input.seekg(_next, std::ios::beg);
        }
        return _metadata;
    }

    audio_metadata _inspect_ogg_tags(const std::filesystem::path& path)
    {
#ifdef _WIN32
        FILE* _file = nullptr;
        _wfopen_s(&_file, path.c_str(), L"rb");
#else
        FILE* _file = std::fopen(path.u8string().c_str(), "rb");
#endif
        if (_file == nullptr) {
            throw audio_error("Could not open Ogg file: " + path.u8string());
        }

        int _error = 0;
        stb_vorbis* _decoder = stb_vorbis_open_file(_file, 0, &_error, nullptr);
        if (_decoder == nullptr) {
            std::fclose(_file);
            throw audio_error("Could not inspect Ogg file: " + path.u8string());
        }

        audio_metadata _metadata;
        const stb_vorbis_comment _comments = stb_vorbis_get_comment(_decoder);
        for (int _index = 0; _index < _comments.comment_list_length; ++_index) {
            if (_comments.comment_list[_index] != nullptr) {
                _parse_vorbis_comment(_metadata, _comments.comment_list[_index]);
            }
        }
        const stb_vorbis_info _info = stb_vorbis_get_info(_decoder);
        _metadata.duration_ms = _duration_milliseconds(
            stb_vorbis_stream_length_in_samples(_decoder),
            _info.sample_rate);
        stb_vorbis_close(_decoder);
        std::fclose(_file);
        return _metadata;
    }

    std::uint64_t _inspect_duration(const std::filesystem::path& path)
    {
        ma_decoder _decoder { };
        const ma_decoder_config _configuration = ma_decoder_config_init_default();
#ifdef _WIN32
        const ma_result _opened = ma_decoder_init_file_w(path.c_str(), &_configuration, &_decoder);
#else
        const std::string _file_name = path.u8string();
        const ma_result _opened = ma_decoder_init_file(_file_name.c_str(), &_configuration, &_decoder);
#endif
        if (_opened != MA_SUCCESS) {
            throw audio_error("Could not decode audio file: " + path.u8string());
        }
        ma_uint64 _frames = 0;
        const ma_result _measured = ma_decoder_get_length_in_pcm_frames(&_decoder, &_frames);
        const std::uint32_t _sample_rate = _decoder.outputSampleRate;
        ma_decoder_uninit(&_decoder);
        if (_measured != MA_SUCCESS) {
            throw audio_error("Could not determine audio duration: " + path.u8string());
        }
        return _duration_milliseconds(_frames, _sample_rate);
    }
}

std::string_view audio_extension_name(audio_extension extension) noexcept
{
    switch (extension) {
    case audio_extension::mp3:
        return "mp3";
    case audio_extension::wav:
        return "wav";
    case audio_extension::flac:
        return "flac";
    case audio_extension::ogg:
        return "ogg";
    case audio_extension::unknown:
        return { };
    }
    return { };
}

int audio_extension_priority(audio_extension extension) noexcept
{
    switch (extension) {
    case audio_extension::flac:
        return 4;
    case audio_extension::wav:
        return 3;
    case audio_extension::ogg:
        return 2;
    case audio_extension::mp3:
        return 1;
    case audio_extension::unknown:
        return 0;
    }
    return 0;
}

std::optional<audio_extension> parse_audio_extension(std::string_view extension) noexcept
{
    if (!extension.empty() && extension.front() == '.') {
        extension.remove_prefix(1);
    }

    char _normalized[4] { };
    if (extension.empty() || extension.size() > sizeof(_normalized)) {
        return std::nullopt;
    }
    for (std::size_t _index = 0; _index < extension.size(); ++_index) {
        const unsigned char _character = static_cast<unsigned char>(extension[_index]);
        _normalized[_index] = static_cast<char>(std::tolower(_character));
    }

    const std::string_view _value(_normalized, extension.size());
    if (_value == "mp3") {
        return audio_extension::mp3;
    }
    if (_value == "wav") {
        return audio_extension::wav;
    }
    if (_value == "flac") {
        return audio_extension::flac;
    }
    if (_value == "ogg") {
        return audio_extension::ogg;
    }
    return std::nullopt;
}

std::optional<audio_extension> audio_extension_from_path(const std::filesystem::path& path) noexcept
{
    try {
        return parse_audio_extension(path.extension().u8string());
    } catch (...) {
        return std::nullopt;
    }
}

std::string_view audio_content_type(audio_extension extension) noexcept
{
    switch (extension) {
    case audio_extension::mp3:
        return "audio/mpeg";
    case audio_extension::wav:
        return "audio/wav";
    case audio_extension::flac:
        return "audio/flac";
    case audio_extension::ogg:
        return "audio/ogg";
    case audio_extension::unknown:
        return "application/octet-stream";
    }
    return "application/octet-stream";
}

audio_metadata inspect_audio_file(const std::filesystem::path& path, audio_extension extension)
{
    audio_metadata _metadata;
    switch (extension) {
    case audio_extension::mp3:
        _metadata = _inspect_mp3_tags(path);
        break;
    case audio_extension::wav:
        _metadata = _inspect_wav_tags(path);
        break;
    case audio_extension::flac:
        _metadata = _inspect_flac_tags(path);
        break;
    case audio_extension::ogg:
        _metadata = _inspect_ogg_tags(path);
        break;
    default:
        throw audio_error("Audio extension is not supported");
    }

    _metadata.duration_ms = _inspect_duration(path);
    if (_metadata.title.empty()) {
        _metadata.title = path.stem().u8string();
    }
    return _metadata;
}

struct file_audio_source::implementation {
    explicit implementation(const std::filesystem::path& path)
        : _stream(path, std::ios::binary)
    {
        if (!_stream) {
            throw audio_error("Could not open audio file: " + path.u8string());
        }

        _stream.seekg(0, std::ios::end);
        const std::streampos _end = _stream.tellg();
        if (_end < 0) {
            throw audio_error("Could not determine audio file size: " + path.u8string());
        }

        _length = static_cast<std::uint64_t>(_end);
        _stream.seekg(0, std::ios::beg);
        if (!_stream) {
            throw audio_error("Could not seek audio file: " + path.u8string());
        }
    }

    std::ifstream _stream;
    std::uint64_t _position { 0 };
    std::uint64_t _length { 0 };
};

file_audio_source::file_audio_source(std::filesystem::path path)
    : _implementation(std::make_unique<implementation>(path))
{
}

file_audio_source::~file_audio_source() = default;
file_audio_source::file_audio_source(file_audio_source&& other) noexcept = default;
file_audio_source& file_audio_source::operator=(file_audio_source&& other) noexcept = default;

std::size_t file_audio_source::read(std::byte* destination, std::size_t size)
{
    if (destination == nullptr && size != 0) {
        throw audio_error("Cannot read audio into a null destination");
    }
    if (size == 0) {
        return 0;
    }

    const std::size_t _maximum = static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)());
    const std::streamsize _requested = static_cast<std::streamsize>((std::min)(size, _maximum));
    _implementation->_stream.read(reinterpret_cast<char*>(destination), _requested);

    const std::streamsize _bytes_read = _implementation->_stream.gcount();
    if (_implementation->_stream.bad()) {
        throw audio_error("Could not read audio data");
    }

    _implementation->_position += static_cast<std::uint64_t>(_bytes_read);
    return static_cast<std::size_t>(_bytes_read);
}

bool file_audio_source::seek(std::uint64_t offset)
{
    if (offset > _implementation->_length || offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
        return false;
    }

    _implementation->_stream.clear();
    _implementation->_stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!_implementation->_stream) {
        return false;
    }

    _implementation->_position = offset;
    return true;
}

std::uint64_t file_audio_source::tell() const
{
    return _implementation->_position;
}

std::uint64_t file_audio_source::size() const
{
    return _implementation->_length;
}

struct stream_audio_source::implementation {
    implementation(
        std::shared_ptr<soundstep::peer_client> requested_client,
        std::string requested_hash,
        std::uint64_t requested_size,
        std::size_t requested_read_ahead_size)
        : _client(std::move(requested_client))
        , _hash(std::move(requested_hash))
        , _total_size(requested_size)
        , _read_ahead_size(requested_read_ahead_size)
    {
        if (_client == nullptr) {
            throw audio_error("Cannot stream audio from a null peer");
        }
        if (!_is_audio_asset_hash(_hash)) {
            throw audio_error("Audio asset hash must contain 64 hexadecimal characters");
        }
        if (_total_size == 0) {
            throw audio_error("Audio asset size must be greater than zero");
        }
        if (_read_ahead_size == 0) {
            throw audio_error("Audio read-ahead size must be greater than zero");
        }
    }

    void refill()
    {
        const std::uint64_t _remaining = _total_size - _cursor;
        const std::uint64_t _requested64 = (std::min)(_remaining,
            static_cast<std::uint64_t>(_read_ahead_size));
        const std::size_t _requested = static_cast<std::size_t>(_requested64);

        _buffer.clear();
        _buffer.reserve(_requested);
        bool _oversized_response = false;
        peer_response _response = _client->stream_asset(
            _hash,
            [this, _requested, &_oversized_response](const char* data, std::size_t size) {
                if (size > _requested - _buffer.size()) {
                    _oversized_response = true;
                    return false;
                }
                if (size == 0) {
                    return true;
                }

                const std::byte* _begin = reinterpret_cast<const std::byte*>(data);
                _buffer.insert(_buffer.end(), _begin, _begin + size);
                return true;
            },
            _cursor,
            _requested64);

        if (_oversized_response) {
            throw audio_error("Peer returned more audio data than requested");
        }
        if (!_response) {
            std::string _message = "Could not read audio from peer";
            if (!_response.error_message.empty()) {
                _message += ": " + _response.error_message;
            } else if (_response.status_code != 0) {
                _message += ": HTTP " + std::to_string(_response.status_code);
            }
            throw audio_error(std::move(_message));
        }
        if (_buffer.size() != _requested) {
            throw audio_error("Peer returned an incomplete audio byte range");
        }

        _buffer_offset = _cursor;
    }

    std::shared_ptr<soundstep::peer_client> _client;
    std::string _hash;
    std::uint64_t _total_size { 0 };
    std::uint64_t _cursor { 0 };
    std::uint64_t _buffer_offset { 0 };
    std::size_t _read_ahead_size { 0 };
    std::vector<std::byte> _buffer;
    mutable std::mutex _mutex;
};

stream_audio_source::stream_audio_source(std::shared_ptr<peer_client> client, std::string hash, std::uint64_t size, std::size_t read_ahead_size)
    : _implementation(std::make_unique<implementation>(
          std::move(client),
          std::move(hash),
          size,
          read_ahead_size))
{
}

stream_audio_source::~stream_audio_source() = default;
stream_audio_source::stream_audio_source(stream_audio_source&& other) noexcept = default;
stream_audio_source& stream_audio_source::operator=(stream_audio_source&& other) noexcept = default;

std::size_t stream_audio_source::read(std::byte* destination, std::size_t size)
{
    if (destination == nullptr && size != 0) {
        throw audio_error("Cannot read streamed audio into a null destination");
    }
    if (size == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    const std::uint64_t _remaining = _implementation->_total_size - _implementation->_cursor;
    const std::size_t _target = static_cast<std::size_t>((std::min)(_remaining,
        static_cast<std::uint64_t>(size)));
    std::size_t _copied = 0;

    while (_copied < _target) {
        const std::uint64_t _buffer_end = _implementation->_buffer_offset + _implementation->_buffer.size();
        const bool _buffered = _implementation->_cursor >= _implementation->_buffer_offset && _implementation->_cursor < _buffer_end;
        if (!_buffered) {
            _implementation->refill();
        }

        const std::size_t _buffer_index = static_cast<std::size_t>(
            _implementation->_cursor - _implementation->_buffer_offset);
        const std::size_t _available = _implementation->_buffer.size() - _buffer_index;
        const std::size_t _count = (std::min)(_available, _target - _copied);
        std::memcpy(
            destination + _copied,
            _implementation->_buffer.data() + _buffer_index,
            _count);
        _copied += _count;
        _implementation->_cursor += _count;
    }

    return _copied;
}

bool stream_audio_source::seek(std::uint64_t offset)
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    if (offset > _implementation->_total_size) {
        return false;
    }
    _implementation->_cursor = offset;
    return true;
}

std::uint64_t stream_audio_source::tell() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    return _implementation->_cursor;
}

std::uint64_t stream_audio_source::size() const
{
    return _implementation->_total_size;
}

}
