#include <algorithm>
#include <array>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#include <sqlite3.h>

#include <core/storage.hpp>

namespace soundstep {
namespace {

    constexpr int schema_version = 1;
    constexpr std::size_t maximum_cover_size = 8 * 1024 * 1024;

    [[nodiscard]] std::string path_string(const std::filesystem::path& path)
    {
        return path.u8string();
    }

    void require_value(bool condition, std::string message)
    {
        if (!condition) {
            throw storage_error(std::move(message));
        }
    }

    std::string normalize_hash(std::string_view hash)
    {
        require_value(hash.size() == 64, "File hash must contain exactly 64 hexadecimal characters");

        std::string _normalized;
        _normalized.reserve(hash.size());
        for (const char _character : hash) {
            if (_character >= '0' && _character <= '9') {
                _normalized += _character;
            } else if (_character >= 'a' && _character <= 'f') {
                _normalized += _character;
            } else if (_character >= 'A' && _character <= 'F') {
                _normalized += static_cast<char>(_character - 'A' + 'a');
            } else {
                throw storage_error("File hash must contain exactly 64 hexadecimal characters");
            }
        }
        return _normalized;
    }

    [[nodiscard]] std::string normalize_fingerprint(std::string_view fingerprint)
    {
        require_value(fingerprint.size() == 64, "Transport fingerprint must contain exactly 64 hexadecimal characters");

        std::string _normalized;
        _normalized.reserve(fingerprint.size());
        for (const char _character : fingerprint) {
            if (_character >= '0' && _character <= '9') {
                _normalized += _character;
            } else if (_character >= 'a' && _character <= 'f') {
                _normalized += _character;
            } else if (_character >= 'A' && _character <= 'F') {
                _normalized += static_cast<char>(_character - 'A' + 'a');
            } else {
                throw storage_error("Transport fingerprint must contain exactly 64 hexadecimal characters");
            }
        }
        return _normalized;
    }

    sqlite3_int64 sqlite_integer(std::uint64_t value, std::string_view field)
    {
        require_value(value <= static_cast<std::uint64_t>((std::numeric_limits<sqlite3_int64>::max)()), std::string(field) + " is too large for SQLite");
        return static_cast<sqlite3_int64>(value);
    }

    [[nodiscard]] std::uint64_t unsigned_integer(sqlite3_int64 value, std::string_view field)
    {
        require_value(value >= 0, std::string(field) + " cannot be negative");
        return static_cast<std::uint64_t>(value);
    }

    [[nodiscard]] std::uint32_t stored_track_number(sqlite3_int64 value)
    {
        const std::uint64_t _number = unsigned_integer(value, "Track number");
        require_value(_number <= (std::numeric_limits<std::uint32_t>::max)(), "Stored track number is too large");
        return static_cast<std::uint32_t>(_number);
    }

    std::string normalized_extension(audio_extension extension)
    {
        const std::string_view _name = audio_extension_name(extension);
        require_value(!_name.empty(), "Audio extension is not supported");
        return std::string(_name);
    }

    [[nodiscard]] audio_extension stored_extension(std::string_view extension)
    {
        const std::optional<audio_extension> _parsed = parse_audio_extension(extension);
        require_value(_parsed.has_value(), "Stored audio extension is missing or unsupported");
        return *_parsed;
    }

    std::string normalized_cover_type(std::string_view content_type)
    {
        require_value(content_type == "image/jpeg" || content_type == "image/png", "Cover image type is unsupported");
        return std::string(content_type);
    }

    struct statement {
        statement(sqlite3* database, const char* sql)
            : _database(database)
        {
            const int _result = sqlite3_prepare_v2(database, sql, -1, &_handle, nullptr);
            if (_result != SQLITE_OK) {
                throw storage_error(std::string("Could not prepare SQLite statement: ") + sqlite3_errmsg(database));
            }
        }

        ~statement()
        {
            sqlite3_finalize(_handle);
        }

        statement(const statement&) = delete;
        statement& operator=(const statement&) = delete;

        void bind(int index, std::string_view value)
        {
            require_value(value.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()), "Text value is too large for SQLite");
            _check(sqlite3_bind_text(_handle, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
        }

        void bind(int index, sqlite3_int64 value)
        {
            _check(sqlite3_bind_int64(_handle, index, value));
        }

        void bind(int index, bool value)
        {
            _check(sqlite3_bind_int(_handle, index, value ? 1 : 0));
        }

        void bind(int index, const std::vector<unsigned char>& value)
        {
            require_value(value.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()), "Binary value is too large for SQLite");
            _check(sqlite3_bind_blob(_handle, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
        }

        bool next()
        {
            const int _result = sqlite3_step(_handle);
            if (_result == SQLITE_ROW) {
                return true;
            }
            if (_result == SQLITE_DONE) {
                return false;
            }
            _check(_result);
            return false;
        }

        void execute()
        {
            require_value(!next(), "SQLite statement unexpectedly returned a row");
        }

        void reset()
        {
            _check(sqlite3_reset(_handle));
            _check(sqlite3_clear_bindings(_handle));
        }

        [[nodiscard]] std::string text(int column) const
        {
            const unsigned char* _value = sqlite3_column_text(_handle, column);
            if (_value == nullptr) {
                return { };
            }
            const int _size = sqlite3_column_bytes(_handle, column);
            return std::string(reinterpret_cast<const char*>(_value), static_cast<std::size_t>(_size));
        }

        [[nodiscard]] sqlite3_int64 integer(int column) const
        {
            return sqlite3_column_int64(_handle, column);
        }

        [[nodiscard]] std::vector<unsigned char> blob(int column) const
        {
            const void* _value = sqlite3_column_blob(_handle, column);
            const int _size = sqlite3_column_bytes(_handle, column);
            if (_value == nullptr || _size <= 0) {
                return { };
            }
            const unsigned char* _begin = static_cast<const unsigned char*>(_value);
            return std::vector<unsigned char>(_begin, _begin + _size);
        }

    private:
        void _check(int result) const
        {
            if (result != SQLITE_OK) {
                throw storage_error(std::string("SQLite statement failed: ") + sqlite3_errmsg(_database));
            }
        }

        sqlite3* _database { nullptr };
        sqlite3_stmt* _handle { nullptr };
    };

    void execute(sqlite3* database, const char* sql)
    {
        char* _error = nullptr;
        const int _result = sqlite3_exec(database, sql, nullptr, nullptr, &_error);
        if (_result != SQLITE_OK) {
            const std::string _message = _error != nullptr ? _error : sqlite3_errmsg(database);
            sqlite3_free(_error);
            throw storage_error("SQLite command failed: " + _message);
        }
    }

    struct transaction {
        explicit transaction(sqlite3* database)
            : _database(database)
        {
            execute(_database, "BEGIN IMMEDIATE");
        }

        ~transaction()
        {
            if (!_committed) {
                sqlite3_exec(_database, "ROLLBACK", nullptr, nullptr, nullptr);
            }
        }

        void commit()
        {
            execute(_database, "COMMIT");
            _committed = true;
        }

    private:
        sqlite3* _database;
        bool _committed { false };
    };

    [[nodiscard]] std::string create_uuid()
    {
        std::array<unsigned char, 16> _bytes { };
        std::random_device _random;
        for (unsigned char& _byte : _bytes) {
            _byte = static_cast<unsigned char>(_random());
        }
        _bytes[6] = static_cast<unsigned char>((_bytes[6] & 0x0f) | 0x40);
        _bytes[8] = static_cast<unsigned char>((_bytes[8] & 0x3f) | 0x80);

        std::ostringstream _output;
        _output << std::hex << std::setfill('0');
        for (std::size_t _index = 0; _index < _bytes.size(); ++_index) {
            if (_index == 4 || _index == 6 || _index == 8 || _index == 10) {
                _output << '-';
            }
            _output << std::setw(2) << static_cast<unsigned int>(_bytes[_index]);
        }
        return _output.str();
    }

    [[nodiscard]] std::string create_access_token()
    {
        return create_uuid() + create_uuid();
    }

    [[nodiscard]] std::string file_sha256(const std::filesystem::path& path)
    {
        try {
            return file_fingerprint(path);
        } catch (const security_error& exception) {
            throw storage_error(exception.what());
        }
    }

    [[nodiscard]] std::string data_sha256(const std::vector<unsigned char>& data)
    {
        try {
            return data_fingerprint(std::string_view(
                reinterpret_cast<const char*>(data.data()),
                data.size()));
        } catch (const security_error& exception) {
            throw storage_error(exception.what());
        }
    }

    [[nodiscard]] bool is_same_track(const track& left, const track& right)
    {
        return std::tie(
                   left.id,
                   left.catalog_id,
                   left.file_hash,
                   left.cover_hash,
                   left.cover_content_type,
                   left.cover_size_bytes,
                   left.extension,
                   left.title,
                   left.artist,
                   left.album,
                   left.track_number,
                   left.duration_ms,
                   left.size_bytes)
            == std::tie(
                right.id,
                right.catalog_id,
                right.file_hash,
                right.cover_hash,
                right.cover_content_type,
                right.cover_size_bytes,
                right.extension,
                right.title,
                right.artist,
                right.album,
                right.track_number,
                right.duration_ms,
                right.size_bytes);
    }

    [[nodiscard]] bool is_setting_bool(const std::optional<std::string>& value, bool fallback)
    {
        if (!value) {
            return fallback;
        }
        require_value(*value == "0" || *value == "1", "Stored boolean setting is invalid");
        return *value == "1";
    }

    struct scanned_asset {
        track catalog_track;
        file_location file;
        std::optional<cover_art> cover;
    };

    [[nodiscard]] track read_track(const statement& query)
    {
        return {
            query.text(0),
            query.text(1),
            query.text(2),
            query.text(3),
            query.text(4),
            unsigned_integer(query.integer(5), "Cover size"),
            stored_extension(query.text(6)),
            query.text(7),
            query.text(8),
            query.text(9),
            stored_track_number(query.integer(10)),
            unsigned_integer(query.integer(11), "Track duration"),
            unsigned_integer(query.integer(12), "Track size")
        };
    }

    void validate_track(const track& track)
    {
        require_value(!track.id.empty(), "Track ID cannot be empty");
        require_value(!track.catalog_id.empty(), "Track catalog ID cannot be empty");
        normalize_hash(track.file_hash);
        if (track.cover_hash.empty()) {
            require_value(track.cover_content_type.empty() && track.cover_size_bytes == 0, "Track cover fields are incomplete");
        } else {
            normalize_hash(track.cover_hash);
            normalized_cover_type(track.cover_content_type);
            require_value(track.cover_size_bytes > 0 && track.cover_size_bytes <= maximum_cover_size, "Track cover size is invalid");
        }
        normalized_extension(track.extension);
        sqlite_integer(track.track_number, "Track number");
        sqlite_integer(track.duration_ms, "Track duration");
        sqlite_integer(track.size_bytes, "Track size");
    }

    void bind_track(statement& statement, const track& track)
    {
        statement.bind(1, track.id);
        statement.bind(2, track.catalog_id);
        statement.bind(3, normalize_hash(track.file_hash));
        statement.bind(4, track.cover_hash);
        statement.bind(5, track.cover_content_type);
        statement.bind(6, sqlite_integer(track.cover_size_bytes, "Cover size"));
        statement.bind(7, normalized_extension(track.extension));
        statement.bind(8, track.title);
        statement.bind(9, track.artist);
        statement.bind(10, track.album);
        statement.bind(11, sqlite_integer(track.track_number, "Track number"));
        statement.bind(12, sqlite_integer(track.duration_ms, "Track duration"));
        statement.bind(13, sqlite_integer(track.size_bytes, "Track size"));
    }

}

struct storage::implementation {
    implementation(std::filesystem::path requested_database_path, std::filesystem::path requested_asset_directory)
        : _database_path(std::move(requested_database_path))
        , _asset_directory(std::move(requested_asset_directory))
    {
        require_value(!_database_path.empty(), "Database path cannot be empty");
        require_value(!_asset_directory.empty(), "Asset directory cannot be empty");

        std::error_code _filesystem_error;
        const std::filesystem::path _database_parent = _database_path.parent_path();
        if (!_database_parent.empty()) {
            std::filesystem::create_directories(_database_parent, _filesystem_error);
            if (_filesystem_error) {
                throw storage_error("Could not create database directory: " + _filesystem_error.message());
            }
        }
        std::filesystem::create_directories(_asset_directory / ".partial", _filesystem_error);
        if (_filesystem_error) {
            throw storage_error("Could not create asset directory: " + _filesystem_error.message());
        }

        sqlite3* _opened_database = nullptr;
        const int _result = sqlite3_open_v2(path_string(_database_path).c_str(), &_opened_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        _database = _opened_database;
        if (_result != SQLITE_OK) {
            const std::string _message = _database != nullptr ? sqlite3_errmsg(_database) : "unknown error";
            sqlite3_close_v2(_database);
            _database = nullptr;
            throw storage_error("Could not open SQLite database: " + _message);
        }

        try {
            sqlite3_busy_timeout(_database, 5000);
            execute(_database, "PRAGMA foreign_keys = ON");
            execute(_database, "PRAGMA journal_mode = WAL");
            execute(_database, "PRAGMA synchronous = NORMAL");
            initialize_schema();
            ensure_identity();
        } catch (...) {
            sqlite3_close_v2(_database);
            _database = nullptr;
            throw;
        }
    }

    ~implementation()
    {
        sqlite3_close_v2(_database);
    }

    void initialize_schema()
    {
        int _version = 0;
        {
            statement _version_query(_database, "PRAGMA user_version");
            require_value(_version_query.next(), "Could not read SQLite schema version");
            _version = static_cast<int>(_version_query.integer(0));
        }
        require_value(_version == 0 || _version == schema_version, "Soundstep's development database schema changed; remove soundstep.db and restart");

        if (_version == 0) {
            transaction _creation(_database);
            execute(_database, R"sql(
                CREATE TABLE settings (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );

                CREATE TABLE catalogs (
                    id TEXT PRIMARY KEY,
                    owner_instance_id TEXT NOT NULL,
                    name TEXT NOT NULL,
                    revision INTEGER NOT NULL DEFAULT 0
                );

                CREATE TABLE tracks (
                    id TEXT NOT NULL,
                    catalog_id TEXT NOT NULL,
                    file_hash TEXT NOT NULL CHECK (length(file_hash) = 64),
                    cover_hash TEXT NOT NULL DEFAULT '' CHECK (cover_hash = '' OR length(cover_hash) = 64),
                    cover_content_type TEXT NOT NULL DEFAULT '' CHECK (cover_content_type IN ('', 'image/jpeg', 'image/png')),
                    cover_size INTEGER NOT NULL DEFAULT 0 CHECK (cover_size >= 0 AND cover_size <= 8388608),
                    extension TEXT NOT NULL CHECK (extension IN ('mp3', 'wav', 'flac', 'ogg')),
                    title TEXT NOT NULL,
                    artist TEXT NOT NULL DEFAULT '',
                    album TEXT NOT NULL DEFAULT '',
                    track_number INTEGER NOT NULL DEFAULT 0 CHECK (track_number >= 0 AND track_number <= 4294967295),
                    duration_ms INTEGER NOT NULL DEFAULT 0 CHECK (duration_ms >= 0),
                    size INTEGER NOT NULL CHECK (size >= 0),
                    PRIMARY KEY (catalog_id, id),
                    FOREIGN KEY (catalog_id) REFERENCES catalogs(id) ON DELETE CASCADE
                );

                CREATE INDEX tracks_by_file_hash ON tracks(file_hash);
                CREATE INDEX tracks_by_cover_hash ON tracks(cover_hash);

                CREATE TABLE covers (
                    hash TEXT PRIMARY KEY CHECK (length(hash) = 64),
                    content_type TEXT NOT NULL CHECK (content_type IN ('image/jpeg', 'image/png')),
                    data BLOB NOT NULL CHECK (length(data) > 0 AND length(data) <= 8388608)
                );

                CREATE TABLE files (
                    hash TEXT PRIMARY KEY CHECK (length(hash) = 64),
                    locator TEXT NOT NULL,
                    extension TEXT NOT NULL CHECK (extension IN ('mp3', 'wav', 'flac', 'ogg')),
                    kind INTEGER NOT NULL CHECK (kind IN (0, 1)),
                    size INTEGER NOT NULL CHECK (size >= 0)
                );

                CREATE TABLE peers (
                    id TEXT PRIMARY KEY,
                    name TEXT NOT NULL,
                    token TEXT NOT NULL,
                    fingerprint TEXT NOT NULL CHECK (fingerprint = '' OR length(fingerprint) = 64),
                    origin INTEGER NOT NULL CHECK (origin IN (0, 1)),
                    last_seen_ms INTEGER NOT NULL CHECK (last_seen_ms >= 0),
                    library_enabled INTEGER NOT NULL DEFAULT 1 CHECK (library_enabled IN (0, 1))
                );

                CREATE TABLE peer_endpoints (
                    peer_id TEXT NOT NULL,
                    host TEXT NOT NULL,
                    port INTEGER NOT NULL CHECK (port > 0 AND port <= 65535),
                    family INTEGER NOT NULL CHECK (family IN (0, 1)),
                    last_seen_ms INTEGER NOT NULL CHECK (last_seen_ms >= 0),
                    last_success_ms INTEGER NOT NULL CHECK (last_success_ms >= 0),
                    PRIMARY KEY (peer_id, host, port),
                    FOREIGN KEY (peer_id) REFERENCES peers(id) ON DELETE CASCADE
                );

                CREATE TABLE access_grants (
                    token TEXT PRIMARY KEY,
                    peer_id TEXT UNIQUE,
                    FOREIGN KEY (peer_id) REFERENCES peers(id) ON DELETE CASCADE
                );

                CREATE TABLE offline_tracks (
                    catalog_id TEXT NOT NULL,
                    track_id TEXT NOT NULL,
                    PRIMARY KEY (catalog_id, track_id)
                );

                CREATE TABLE track_metadata_overrides (
                    catalog_id TEXT NOT NULL,
                    track_id TEXT NOT NULL,
                    title TEXT NOT NULL,
                    artist TEXT NOT NULL,
                    album TEXT NOT NULL,
                    track_number INTEGER NOT NULL CHECK (track_number >= 0 AND track_number <= 4294967295),
                    PRIMARY KEY (catalog_id, track_id)
                );

                PRAGMA user_version = 1;
            )sql");
            _creation.commit();
        }
    }

    std::optional<std::string> setting(std::string_view key) const
    {
        statement _query(_database, "SELECT value FROM settings WHERE key = ?1");
        _query.bind(1, key);
        if (!_query.next()) {
            return std::nullopt;
        }
        return _query.text(0);
    }

    void write_setting(std::string_view key, std::string_view value)
    {
        statement _query(_database, R"sql(
            INSERT INTO settings(key, value) VALUES(?1, ?2)
            ON CONFLICT(key) DO UPDATE SET value = excluded.value
        )sql");
        _query.bind(1, key);
        _query.bind(2, value);
        _query.execute();
    }

    void ensure_identity()
    {
        if (!setting("instance_id")) {
            write_setting("instance_id", create_uuid());
        }
        if (!setting("instance_name")) {
            write_setting("instance_name", "Soundstep");
        }
        if (!setting("lan_token")) {
            write_setting("lan_token", create_access_token());
        }

        const std::string _id = *setting("instance_id");
        const std::string _name = *setting("instance_name");
        statement _catalog(_database, R"sql(
            INSERT INTO catalogs(id, owner_instance_id, name, revision)
            VALUES(?1, ?1, ?2, 0)
            ON CONFLICT(id) DO UPDATE SET
                owner_instance_id = excluded.owner_instance_id,
                name = excluded.name
        )sql");
        _catalog.bind(1, _id);
        _catalog.bind(2, _name);
        _catalog.execute();
    }

    sqlite3* _database { nullptr };
    std::filesystem::path _database_path;
    std::filesystem::path _asset_directory;
    mutable std::mutex _mutex;
};

storage::storage(std::filesystem::path database_path, std::filesystem::path asset_directory)
    : _implementation(std::make_unique<implementation>(std::move(database_path), std::move(asset_directory)))
{
}

storage::~storage() = default;

instance_info storage::instance() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    const std::optional<std::string> _id = _implementation->setting("instance_id");
    const std::optional<std::string> _name = _implementation->setting("instance_name");
    require_value(_id.has_value() && _name.has_value(), "Storage instance identity is missing");
    return { *_id, *_name };
}

std::string storage::lan_token() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    const std::optional<std::string> _token = _implementation->setting("lan_token");
    require_value(_token.has_value() && !_token->empty(), "Storage LAN token is missing");
    return *_token;
}

std::string storage::create_invitation_token()
{
    const std::string _token = create_access_token();
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);
    execute(_implementation->_database, "DELETE FROM access_grants WHERE peer_id IS NULL");
    statement _insert(_implementation->_database, "INSERT INTO access_grants(token, peer_id) VALUES(?1, NULL)");
    _insert.bind(1, _token);
    _insert.execute();
    _update.commit();
    return _token;
}

std::string storage::access_token_for_peer(std::string_view peer_id)
{
    require_value(!peer_id.empty(), "Peer ID cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _existing(_implementation->_database, "SELECT token FROM access_grants WHERE peer_id = ?1");
    _existing.bind(1, peer_id);
    if (_existing.next()) {
        return _existing.text(0);
    }

    const std::string _token = create_access_token();
    statement _insert(_implementation->_database, "INSERT INTO access_grants(token, peer_id) VALUES(?1, ?2)");
    _insert.bind(1, _token);
    _insert.bind(2, peer_id);
    _insert.execute();
    return _token;
}

bool storage::authorize_access(std::string_view token) const
{
    if (token.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    const std::optional<std::string> _lan = _implementation->setting("lan_token");
    if (_lan && token == *_lan) {
        return true;
    }
    statement _query(_implementation->_database, "SELECT 1 FROM access_grants WHERE token = ?1");
    _query.bind(1, token);
    return _query.next();
}

void storage::claim_invitation(std::string_view token, std::string_view peer_id)
{
    require_value(!token.empty(), "Invitation token cannot be empty");
    require_value(!peer_id.empty(), "Peer ID cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, "SELECT peer_id FROM access_grants WHERE token = ?1");
    _query.bind(1, token);
    require_value(_query.next(), "Invitation token is invalid or revoked");
    const std::string _claimed = _query.text(0);
    require_value(_claimed.empty() || _claimed == peer_id, "Invitation token was already claimed by another instance");
    if (!_claimed.empty()) {
        return;
    }
    statement _claim(_implementation->_database, "UPDATE access_grants SET peer_id = ?2 WHERE token = ?1 AND peer_id IS NULL");
    _claim.bind(1, token);
    _claim.bind(2, peer_id);
    _claim.execute();
    require_value(sqlite3_changes(_implementation->_database) == 1, "Invitation token could not be claimed");
}

std::optional<soundstep::transport_identity> storage::transport_identity() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    const std::optional<std::string> _certificate = _implementation->setting("transport_certificate");
    const std::optional<std::string> _private_key = _implementation->setting("transport_private_key");
    const std::optional<std::string> _fingerprint = _implementation->setting("transport_fingerprint");
    if (!_certificate && !_private_key && !_fingerprint) {
        return std::nullopt;
    }
    require_value(_certificate && !_certificate->empty() && _private_key && !_private_key->empty() && _fingerprint && _fingerprint->size() == 64, "Stored transport identity is incomplete");
    require_value(transport_certificate_fingerprint(*_certificate) == *_fingerprint, "Stored transport certificate fingerprint does not match");
    return soundstep::transport_identity { *_certificate, *_private_key, *_fingerprint };
}

void storage::set_transport_identity(const soundstep::transport_identity& identity)
{
    require_value(!identity.certificate_pem.empty(), "Transport certificate cannot be empty");
    require_value(!identity.private_key_pem.empty(), "Transport private key cannot be empty");
    require_value(identity.fingerprint.size() == 64, "Transport fingerprint must contain 64 hexadecimal characters");
    require_value(transport_certificate_fingerprint(identity.certificate_pem) == identity.fingerprint, "Transport certificate fingerprint does not match");

    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);
    _implementation->write_setting("transport_certificate", identity.certificate_pem);
    _implementation->write_setting("transport_private_key", identity.private_key_pem);
    _implementation->write_setting("transport_fingerprint", identity.fingerprint);
    _update.commit();
}

void storage::set_instance_name(std::string name)
{
    require_value(!name.empty(), "Instance name cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);
    _implementation->write_setting("instance_name", name);

    const std::string _id = *_implementation->setting("instance_id");
    statement _rename_catalog(_implementation->_database, "UPDATE catalogs SET name = ?2, revision = revision + 1 WHERE id = ?1");
    _rename_catalog.bind(1, _id);
    _rename_catalog.bind(2, name);
    _rename_catalog.execute();
    require_value(sqlite3_changes(_implementation->_database) == 1, "Local catalog was not found");
    _update.commit();
}

configuration storage::config() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    const std::optional<std::string> _path = _implementation->setting("library_path");
    return {
        _path ? std::filesystem::u8path(*_path) : std::filesystem::path { },
        is_setting_bool(_implementation->setting("scan_subdirectories"), true),
        is_setting_bool(_implementation->setting("scan_on_startup"), true),
        is_setting_bool(_implementation->setting("lan_discovery_enabled"), true)
    };
}

void storage::set_config(const configuration& config)
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);
    _implementation->write_setting("library_path", config.library_path.u8string());
    _implementation->write_setting("scan_subdirectories", config.scan_subdirectories ? "1" : "0");
    _implementation->write_setting("scan_on_startup", config.scan_on_startup ? "1" : "0");
    _implementation->write_setting("lan_discovery_enabled", config.lan_discovery_enabled ? "1" : "0");
    _update.commit();
}

std::optional<track> storage::playback_selection() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    const std::optional<std::string> _catalog_id = _implementation->setting("playback_catalog_id");
    const std::optional<std::string> _track_id = _implementation->setting("playback_track_id");
    if (!_catalog_id || !_track_id || _catalog_id->empty() || _track_id->empty()) {
        return std::nullopt;
    }

    statement _query(_implementation->_database, R"sql(
        SELECT id, catalog_id, file_hash, cover_hash, cover_content_type, cover_size,
               extension, title, artist, album, track_number, duration_ms, size
        FROM tracks
        WHERE catalog_id = ?1 AND id = ?2
    )sql");
    _query.bind(1, *_catalog_id);
    _query.bind(2, *_track_id);
    if (!_query.next()) {
        return std::nullopt;
    }
    return read_track(_query);
}

void storage::set_playback_selection(const track& selected_track)
{
    require_value(!selected_track.catalog_id.empty(), "Selected track catalog ID cannot be empty");
    require_value(!selected_track.id.empty(), "Selected track ID cannot be empty");

    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    {
        statement _existing(_implementation->_database, "SELECT 1 FROM tracks WHERE catalog_id = ?1 AND id = ?2");
        _existing.bind(1, selected_track.catalog_id);
        _existing.bind(2, selected_track.id);
        require_value(_existing.next(), "Selected track was not found");
    }

    transaction _update(_implementation->_database);
    _implementation->write_setting("playback_catalog_id", selected_track.catalog_id);
    _implementation->write_setting("playback_track_id", selected_track.id);
    _update.commit();
}

std::vector<peer_record> storage::peers() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, R"sql(
        SELECT p.id, p.name, p.token, p.fingerprint, p.origin, p.last_seen_ms,
               p.library_enabled,
               e.host, e.port, e.family, e.last_seen_ms, e.last_success_ms
        FROM peers AS p
        LEFT JOIN peer_endpoints AS e ON e.peer_id = p.id
        ORDER BY p.name, p.id, e.last_success_ms DESC, e.last_seen_ms DESC, e.family DESC, e.host
    )sql");

    std::vector<peer_record> _result;
    while (_query.next()) {
        const sqlite3_int64 _origin = _query.integer(4);
        require_value(_origin == 0 || _origin == 1, "Stored peer origin is invalid");
        const sqlite3_int64 _library_enabled = _query.integer(6);
        require_value(_library_enabled == 0 || _library_enabled == 1, "Stored peer library visibility is invalid");
        const std::string _id = _query.text(0);
        if (_result.empty() || _result.back().id != _id) {
            _result.push_back({ _id,
                _query.text(1),
                _query.text(2),
                _query.text(3),
                static_cast<peer_origin>(_origin),
                unsigned_integer(_query.integer(5), "Peer last-seen time"),
                _library_enabled != 0,
                { } });
        }

        const std::string _host = _query.text(7);
        if (!_host.empty()) {
            const sqlite3_int64 _port = _query.integer(8);
            const sqlite3_int64 _family = _query.integer(9);
            require_value(_port > 0 && _port <= 65'535, "Stored peer endpoint port is invalid");
            require_value(_family == 0 || _family == 1, "Stored peer endpoint family is invalid");
            _result.back().endpoints.push_back({ _host,
                static_cast<std::uint16_t>(_port),
                static_cast<peer_endpoint_family>(_family),
                unsigned_integer(_query.integer(10), "Peer endpoint last-seen time"),
                unsigned_integer(_query.integer(11), "Peer endpoint last-success time") });
        }
    }
    return _result;
}

void storage::upsert_peer(const peer_record& peer)
{
    require_value(!peer.id.empty(), "Peer ID cannot be empty");
    require_value(!peer.name.empty(), "Peer name cannot be empty");
    require_value(!peer.token.empty(), "Peer token cannot be empty");
    const std::string _fingerprint = normalize_fingerprint(peer.fingerprint);
    require_value(peer.origin == peer_origin::lan || peer.origin == peer_origin::pairing_code, "Peer origin is invalid");
    for (const peer_endpoint& _endpoint : peer.endpoints) {
        require_value(!_endpoint.host.empty(), "Peer endpoint host cannot be empty");
        require_value(_endpoint.port != 0, "Peer endpoint port cannot be zero");
        require_value(_endpoint.family == peer_endpoint_family::ipv4 || _endpoint.family == peer_endpoint_family::ipv6, "Peer endpoint family is invalid");
        sqlite_integer(_endpoint.last_seen_ms, "Peer endpoint last-seen time");
        sqlite_integer(_endpoint.last_success_ms, "Peer endpoint last-success time");
    }

    const instance_info _local = instance();
    require_value(peer.id != _local.id, "The local instance cannot be stored as a peer");

    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);
    statement _upsert(_implementation->_database, R"sql(
        INSERT INTO peers(id, name, token, fingerprint, origin, last_seen_ms, library_enabled)
        VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)
        ON CONFLICT(id) DO UPDATE SET
            name = excluded.name,
            token = excluded.token,
            fingerprint = excluded.fingerprint,
            origin = CASE
                WHEN peers.origin = 1 THEN peers.origin
                ELSE excluded.origin
            END,
            last_seen_ms = excluded.last_seen_ms
    )sql");
    _upsert.bind(1, peer.id);
    _upsert.bind(2, peer.name);
    _upsert.bind(3, peer.token);
    _upsert.bind(4, _fingerprint);
    _upsert.bind(5, static_cast<sqlite3_int64>(peer.origin));
    _upsert.bind(6, sqlite_integer(peer.last_seen_ms, "Peer last-seen time"));
    _upsert.bind(7, peer.library_enabled);
    _upsert.execute();

    statement _remove_endpoints(_implementation->_database, "DELETE FROM peer_endpoints WHERE peer_id = ?1");
    _remove_endpoints.bind(1, peer.id);
    _remove_endpoints.execute();

    statement _insert_endpoint(_implementation->_database, R"sql(
        INSERT INTO peer_endpoints(
            peer_id, host, port, family, last_seen_ms, last_success_ms)
        VALUES(?1, ?2, ?3, ?4, ?5, ?6)
        ON CONFLICT(peer_id, host, port) DO UPDATE SET
            family = excluded.family,
            last_seen_ms = max(peer_endpoints.last_seen_ms, excluded.last_seen_ms),
            last_success_ms = max(peer_endpoints.last_success_ms, excluded.last_success_ms)
    )sql");
    for (const peer_endpoint& _endpoint : peer.endpoints) {
        _insert_endpoint.bind(1, peer.id);
        _insert_endpoint.bind(2, _endpoint.host);
        _insert_endpoint.bind(3, static_cast<sqlite3_int64>(_endpoint.port));
        _insert_endpoint.bind(4, static_cast<sqlite3_int64>(_endpoint.family));
        _insert_endpoint.bind(5, sqlite_integer(_endpoint.last_seen_ms, "Peer endpoint last-seen time"));
        _insert_endpoint.bind(6, sqlite_integer(_endpoint.last_success_ms, "Peer endpoint last-success time"));
        _insert_endpoint.execute();
        _insert_endpoint.reset();
    }
    _update.commit();
}

void storage::mark_peer_endpoint_success(std::string_view id, const peer_endpoint& endpoint)
{
    require_value(!id.empty(), "Peer ID cannot be empty");
    require_value(!endpoint.host.empty(), "Peer endpoint host cannot be empty");
    require_value(endpoint.port != 0, "Peer endpoint port cannot be zero");

    const std::uint64_t _now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _update(_implementation->_database, R"sql(
        UPDATE peer_endpoints
        SET last_seen_ms = max(last_seen_ms, ?4),
            last_success_ms = max(last_success_ms, ?4)
        WHERE peer_id = ?1 AND host = ?2 AND port = ?3
    )sql");
    _update.bind(1, id);
    _update.bind(2, endpoint.host);
    _update.bind(3, static_cast<sqlite3_int64>(endpoint.port));
    _update.bind(4, sqlite_integer(_now, "Peer endpoint success time"));
    _update.execute();
}

void storage::set_peer_library_enabled(std::string_view id, bool enabled)
{
    require_value(!id.empty(), "Peer ID cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _update(_implementation->_database, "UPDATE peers SET library_enabled = ?2 WHERE id = ?1");
    _update.bind(1, id);
    _update.bind(2, enabled);
    _update.execute();
    require_value(sqlite3_changes(_implementation->_database) == 1, "Peer was not found");
}

void storage::remove_peer(std::string_view id)
{
    require_value(!id.empty(), "Peer ID cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);
    statement _remove_catalog(_implementation->_database, "DELETE FROM catalogs WHERE id = ?1 AND owner_instance_id = ?1");
    _remove_catalog.bind(1, id);
    _remove_catalog.execute();

    statement _remove_peer(_implementation->_database, "DELETE FROM peers WHERE id = ?1");
    _remove_peer.bind(1, id);
    _remove_peer.execute();
    _update.commit();
}

library_scan_result storage::scan_library()
{
    const configuration _settings = config();
    require_value(!_settings.library_path.empty(), "A music folder must be configured before scanning");

    std::error_code _filesystem_error;
    const std::filesystem::path _root = std::filesystem::weakly_canonical(_settings.library_path, _filesystem_error);
    require_value(!_filesystem_error && std::filesystem::is_directory(_root, _filesystem_error) && !_filesystem_error, "The configured music folder does not exist or is not accessible");

    library_scan_result _result;
    std::map<std::string, scanned_asset> _assets;
    const instance_info _local_instance = instance();

    const std::function<void(const std::filesystem::directory_entry&)> _inspect = [&](const std::filesystem::directory_entry& entry) {
        std::error_code _entry_error;
        if (!entry.is_regular_file(_entry_error) || _entry_error) {
            return;
        }

        const std::optional<audio_extension> _extension = audio_extension_from_path(entry.path());
        if (!_extension) {
            return;
        }
        ++_result.files_found;

        const std::uint64_t _size = entry.file_size(_entry_error);
        if (_entry_error || _size == 0) {
            ++_result.files_failed;
            return;
        }

        try {
            const std::string _hash = file_sha256(entry.path());
            const audio_metadata _metadata = inspect_audio_file(entry.path(), *_extension);
            std::optional<cover_art> _cover;
            if (!_metadata.cover_bytes.empty()) {
                _cover = cover_art {
                    data_sha256(_metadata.cover_bytes),
                    _metadata.cover_content_type,
                    _metadata.cover_bytes
                };
            }
            scanned_asset _asset {
                { _hash,
                    _local_instance.id,
                    _hash,
                    _cover ? _cover->hash : std::string { },
                    _cover ? _cover->content_type : std::string { },
                    _cover ? _cover->bytes.size() : 0,
                    *_extension,
                    _metadata.title,
                    _metadata.artist,
                    _metadata.album,
                    _metadata.track_number,
                    _metadata.duration_ms,
                    _size },
                { _hash,
                    entry.path().u8string(),
                    *_extension,
                    file_kind::external,
                    _size },
                std::move(_cover)
            };

            const std::map<std::string, scanned_asset>::iterator _existing = _assets.find(_hash);
            if (_existing == _assets.end() || _asset.file.path < _existing->second.file.path) {
                _assets.insert_or_assign(_hash, std::move(_asset));
            }
        } catch (...) {
            ++_result.files_failed;
        }
    };

    const std::filesystem::directory_options _options = std::filesystem::directory_options::skip_permission_denied;
    if (_settings.scan_subdirectories) {
        std::filesystem::recursive_directory_iterator _iterator(_root, _options, _filesystem_error);
        const std::filesystem::recursive_directory_iterator _end;
        require_value(!_filesystem_error, "Could not read the configured music folder");
        while (_iterator != _end) {
            _inspect(*_iterator);
            _iterator.increment(_filesystem_error);
            if (_filesystem_error) {
                ++_result.files_failed;
                break;
            }
        }
    } else {
        std::filesystem::directory_iterator _iterator(_root, _options, _filesystem_error);
        const std::filesystem::directory_iterator _end;
        require_value(!_filesystem_error, "Could not read the configured music folder");
        while (_iterator != _end) {
            _inspect(*_iterator);
            _iterator.increment(_filesystem_error);
            if (_filesystem_error) {
                ++_result.files_failed;
                break;
            }
        }
    }

    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);

    std::map<std::string, track> _previous_tracks;
    statement _previous(_implementation->_database, R"sql(
        SELECT id, catalog_id, file_hash, cover_hash, cover_content_type, cover_size,
               extension, title, artist, album, track_number, duration_ms, size
        FROM tracks
        WHERE catalog_id = ?1
    )sql");
    _previous.bind(1, _local_instance.id);
    while (_previous.next()) {
        track _value = read_track(_previous);
        _previous_tracks.emplace(_value.id, std::move(_value));
    }

    if (_result.files_failed != 0) {
        statement _previous_file(_implementation->_database, "SELECT locator, extension, kind, size FROM files WHERE hash = ?1");
        for (const std::pair<const std::string, track>& _item : _previous_tracks) {
            if (_assets.find(_item.first) != _assets.end()) {
                continue;
            }

            _previous_file.bind(1, _item.second.file_hash);
            if (_previous_file.next()) {
                _assets.emplace(_item.first, scanned_asset { _item.second, { _item.second.file_hash, _previous_file.text(0), stored_extension(_previous_file.text(1)), static_cast<file_kind>(_previous_file.integer(2)), unsigned_integer(_previous_file.integer(3), "File size") }, std::nullopt });
            }
            _previous_file.reset();
        }
    }

    bool _catalog_changed = _previous_tracks.size() != _assets.size();
    for (const std::pair<const std::string, scanned_asset>& _item : _assets) {
        const std::map<std::string, track>::iterator _previous_track = _previous_tracks.find(_item.first);
        if (_previous_track == _previous_tracks.end()) {
            ++_result.tracks_added;
            _catalog_changed = true;
        } else if (!is_same_track(_previous_track->second, _item.second.catalog_track)) {
            _catalog_changed = true;
        }
    }

    statement _upsert_cover(_implementation->_database, R"sql(
        INSERT INTO covers(hash, content_type, data)
        VALUES(?1, ?2, ?3)
        ON CONFLICT(hash) DO NOTHING
    )sql");
    for (const std::pair<const std::string, scanned_asset>& _item : _assets) {
        if (!_item.second.cover) {
            continue;
        }
        _upsert_cover.bind(1, _item.second.cover->hash);
        _upsert_cover.bind(2, _item.second.cover->content_type);
        _upsert_cover.bind(3, _item.second.cover->bytes);
        _upsert_cover.execute();
        _upsert_cover.reset();
    }
    for (const std::pair<const std::string, track>& _item : _previous_tracks) {
        if (_assets.find(_item.first) == _assets.end()) {
            ++_result.tracks_removed;
        }
    }

    statement _upsert_file(_implementation->_database, R"sql(
        INSERT INTO files(hash, locator, extension, kind, size)
        VALUES(?1, ?2, ?3, 0, ?4)
        ON CONFLICT(hash) DO UPDATE SET
            locator = excluded.locator,
            extension = excluded.extension,
            size = excluded.size
        WHERE files.kind = 0
    )sql");
    for (const std::pair<const std::string, scanned_asset>& _item : _assets) {
        _upsert_file.bind(1, _item.second.file.hash);
        _upsert_file.bind(2, _item.second.file.path);
        _upsert_file.bind(3, normalized_extension(_item.second.file.extension));
        _upsert_file.bind(4, sqlite_integer(_item.second.file.size_bytes, "File size"));
        _upsert_file.execute();
        _upsert_file.reset();
    }

    if (_catalog_changed) {
        statement _clear_tracks(_implementation->_database, "DELETE FROM tracks WHERE catalog_id = ?1");
        _clear_tracks.bind(1, _local_instance.id);
        _clear_tracks.execute();

        statement _insert_track(_implementation->_database, R"sql(
            INSERT INTO tracks(
                id, catalog_id, file_hash, cover_hash, cover_content_type, cover_size,
                extension, title, artist, album, track_number, duration_ms, size)
            VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)
        )sql");
        for (const std::pair<const std::string, scanned_asset>& _item : _assets) {
            const track& _value = _item.second.catalog_track;
            bind_track(_insert_track, _value);
            _insert_track.execute();
            _insert_track.reset();
        }

        statement _revise(_implementation->_database, "UPDATE catalogs SET revision = revision + 1 WHERE id = ?1");
        _revise.bind(1, _local_instance.id);
        _revise.execute();
        require_value(sqlite3_changes(_implementation->_database) == 1, "Local catalog was not found");
    }

    execute(_implementation->_database, R"sql(
        DELETE FROM files
        WHERE kind = 0
          AND NOT EXISTS (SELECT 1 FROM tracks WHERE tracks.file_hash = files.hash)
    )sql");
    execute(_implementation->_database, R"sql(
        DELETE FROM covers
        WHERE NOT EXISTS (SELECT 1 FROM tracks WHERE tracks.cover_hash = covers.hash)
    )sql");
    _update.commit();
    return _result;
}

std::optional<std::uint64_t> storage::catalog_revision(std::string_view id) const
{
    require_value(!id.empty(), "Catalog ID cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, "SELECT revision FROM catalogs WHERE id = ?1");
    _query.bind(1, id);
    if (!_query.next()) {
        return std::nullopt;
    }
    return unsigned_integer(_query.integer(0), "Catalog revision");
}

std::optional<catalog_snapshot> storage::catalog(std::string_view id) const
{
    require_value(!id.empty(), "Catalog ID cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);

    statement _catalog_query(_implementation->_database, R"sql(
        SELECT owner_instance_id, name, revision
        FROM catalogs
        WHERE id = ?1
    )sql");
    _catalog_query.bind(1, id);
    if (!_catalog_query.next()) {
        return std::nullopt;
    }

    catalog_snapshot _result;
    _result.id = std::string(id);
    _result.owner_instance_id = _catalog_query.text(0);
    _result.name = _catalog_query.text(1);
    _result.revision = unsigned_integer(_catalog_query.integer(2), "Catalog revision");

    statement _track_query(_implementation->_database, R"sql(
        SELECT t.id, t.catalog_id, t.file_hash, t.cover_hash,
               t.cover_content_type, t.cover_size, t.extension,
               COALESCE(o.title, t.title),
               COALESCE(o.artist, t.artist),
               COALESCE(o.album, t.album),
               COALESCE(o.track_number, t.track_number),
               t.duration_ms, t.size
        FROM tracks AS t
        LEFT JOIN track_metadata_overrides AS o
          ON o.catalog_id = t.catalog_id AND o.track_id = t.id
        WHERE t.catalog_id = ?1
        ORDER BY t.id
    )sql");
    _track_query.bind(1, id);
    while (_track_query.next()) {
        _result.tracks.push_back(read_track(_track_query));
    }
    return _result;
}

void storage::replace_catalog(const catalog_snapshot& catalog)
{
    require_value(!catalog.id.empty(), "Catalog ID cannot be empty");
    require_value(!catalog.owner_instance_id.empty(), "Catalog owner cannot be empty");
    require_value(!catalog.name.empty(), "Catalog name cannot be empty");
    sqlite_integer(catalog.revision, "Catalog revision");
    for (const track& _item : catalog.tracks) {
        validate_track(_item);
        require_value(_item.catalog_id == catalog.id, "Track belongs to a different catalog");
    }

    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);

    statement _upsert_catalog(_implementation->_database, R"sql(
        INSERT INTO catalogs(id, owner_instance_id, name, revision)
        VALUES(?1, ?2, ?3, ?4)
        ON CONFLICT(id) DO UPDATE SET
            owner_instance_id = excluded.owner_instance_id,
            name = excluded.name,
            revision = excluded.revision
    )sql");
    _upsert_catalog.bind(1, catalog.id);
    _upsert_catalog.bind(2, catalog.owner_instance_id);
    _upsert_catalog.bind(3, catalog.name);
    _upsert_catalog.bind(4, sqlite_integer(catalog.revision, "Catalog revision"));
    _upsert_catalog.execute();

    statement _delete_tracks(_implementation->_database, "DELETE FROM tracks WHERE catalog_id = ?1");
    _delete_tracks.bind(1, catalog.id);
    _delete_tracks.execute();

    statement _insert_track(_implementation->_database, R"sql(
        INSERT INTO tracks(
            id, catalog_id, file_hash, cover_hash, cover_content_type, cover_size,
            extension, title, artist, album, track_number, duration_ms, size)
        VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)
    )sql");
    for (const track& _item : catalog.tracks) {
        bind_track(_insert_track, _item);
        _insert_track.execute();
        _insert_track.reset();
    }

    execute(_implementation->_database, R"sql(
        DELETE FROM covers
        WHERE NOT EXISTS (SELECT 1 FROM tracks WHERE tracks.cover_hash = covers.hash)
    )sql");

    _update.commit();
}

void storage::add_track(const track& track)
{
    validate_track(track);
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);
    statement _upsert(_implementation->_database, R"sql(
        INSERT INTO tracks(
            id, catalog_id, file_hash, cover_hash, cover_content_type, cover_size,
            extension, title, artist, album, track_number, duration_ms, size)
        VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)
        ON CONFLICT(catalog_id, id) DO UPDATE SET
            file_hash = excluded.file_hash,
            cover_hash = excluded.cover_hash,
            cover_content_type = excluded.cover_content_type,
            cover_size = excluded.cover_size,
            extension = excluded.extension,
            title = excluded.title,
            artist = excluded.artist,
            album = excluded.album,
            track_number = excluded.track_number,
            duration_ms = excluded.duration_ms,
            size = excluded.size
    )sql");
    bind_track(_upsert, track);
    _upsert.execute();

    statement _revise(_implementation->_database, "UPDATE catalogs SET revision = revision + 1 WHERE id = ?1");
    _revise.bind(1, track.catalog_id);
    _revise.execute();
    require_value(sqlite3_changes(_implementation->_database) == 1, "Track catalog was not found");
    _update.commit();
}

void storage::remove_track(std::string_view catalog_id, std::string_view track_id)
{
    require_value(!catalog_id.empty(), "Catalog ID cannot be empty");
    require_value(!track_id.empty(), "Track ID cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);
    statement _remove(_implementation->_database, "DELETE FROM tracks WHERE catalog_id = ?1 AND id = ?2");
    _remove.bind(1, catalog_id);
    _remove.bind(2, track_id);
    _remove.execute();
    if (sqlite3_changes(_implementation->_database) == 0) {
        _update.commit();
        return;
    }

    statement _revise(_implementation->_database, "UPDATE catalogs SET revision = revision + 1 WHERE id = ?1");
    _revise.bind(1, catalog_id);
    _revise.execute();
    require_value(sqlite3_changes(_implementation->_database) == 1, "Track catalog was not found");
    _update.commit();
}

void storage::update_track_metadata(std::string_view catalog_id, std::string_view track_id, std::string title, std::string artist, std::string album, std::uint32_t track_number)
{
    require_value(!catalog_id.empty(), "Catalog ID cannot be empty");
    require_value(!track_id.empty(), "Track ID cannot be empty");
    constexpr std::size_t _maximum_metadata_size = 1024 * 1024;
    require_value(title.size() <= _maximum_metadata_size && artist.size() <= _maximum_metadata_size && album.size() <= _maximum_metadata_size, "Track metadata is too large");
    sqlite_integer(track_number, "Track number");

    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    transaction _update(_implementation->_database);
    {
        statement _track(_implementation->_database, R"sql(
            SELECT 1 FROM tracks WHERE catalog_id = ?1 AND id = ?2
        )sql");
        _track.bind(1, catalog_id);
        _track.bind(2, track_id);
        require_value(_track.next(), "Track was not found");
    }

    statement _override(_implementation->_database, R"sql(
        INSERT INTO track_metadata_overrides(
            catalog_id, track_id, title, artist, album, track_number)
        VALUES(?1, ?2, ?3, ?4, ?5, ?6)
        ON CONFLICT(catalog_id, track_id) DO UPDATE SET
            title = excluded.title,
            artist = excluded.artist,
            album = excluded.album,
            track_number = excluded.track_number
    )sql");
    _override.bind(1, catalog_id);
    _override.bind(2, track_id);
    _override.bind(3, title);
    _override.bind(4, artist);
    _override.bind(5, album);
    _override.bind(6, sqlite_integer(track_number, "Track number"));
    _override.execute();

    const std::optional<std::string> _local_id = _implementation->setting("instance_id");
    require_value(_local_id.has_value(), "Storage instance identity is missing");
    if (catalog_id == *_local_id) {
        statement _revise(_implementation->_database, "UPDATE catalogs SET revision = revision + 1 WHERE id = ?1");
        _revise.bind(1, catalog_id);
        _revise.execute();
        require_value(sqlite3_changes(_implementation->_database) == 1, "Track catalog was not found");
    }
    _update.commit();
}

std::optional<file_location> storage::find_file(std::string_view hash) const
{
    const std::string _normalized = normalize_hash(hash);
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, "SELECT locator, extension, kind, size FROM files WHERE hash = ?1");
    _query.bind(1, _normalized);
    if (!_query.next()) {
        return std::nullopt;
    }

    const sqlite3_int64 _stored_kind = _query.integer(2);
    require_value(_stored_kind == 0 || _stored_kind == 1, "Stored file kind is invalid");
    return file_location {
        _normalized,
        _query.text(0),
        stored_extension(_query.text(1)),
        static_cast<file_kind>(_stored_kind),
        unsigned_integer(_query.integer(3), "File size")
    };
}

std::optional<cover_art> storage::cover(std::string_view hash) const
{
    const std::string _normalized = normalize_hash(hash);
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, "SELECT content_type, data FROM covers WHERE hash = ?1");
    _query.bind(1, _normalized);
    if (!_query.next()) {
        return std::nullopt;
    }
    cover_art _result { _normalized, _query.text(0), _query.blob(1) };
    normalized_cover_type(_result.content_type);
    require_value(!_result.bytes.empty() && _result.bytes.size() <= maximum_cover_size, "Stored cover data is invalid");
    require_value(data_sha256(_result.bytes) == _normalized, "Stored cover data does not match its hash");
    return _result;
}

std::vector<file_location> storage::managed_files() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, R"sql(
        SELECT hash, locator, extension, size
        FROM files WHERE kind = 1 ORDER BY hash
    )sql");
    std::vector<file_location> _result;
    while (_query.next()) {
        _result.push_back({ _query.text(0),
            _query.text(1),
            stored_extension(_query.text(2)),
            file_kind::managed,
            unsigned_integer(_query.integer(3), "File size") });
    }
    return _result;
}

music_storage_usage storage::music_usage() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, R"sql(
        SELECT
            COALESCE(SUM(CASE WHEN kind = 0 THEN size ELSE 0 END), 0),
            COALESCE(SUM(CASE WHEN kind = 1 THEN size ELSE 0 END), 0)
        FROM files
    )sql");
    require_value(_query.next(), "Could not calculate music storage usage");
    return {
        unsigned_integer(_query.integer(0), "Local music storage usage"),
        unsigned_integer(_query.integer(1), "Downloaded music storage usage")
    };
}

void storage::store_cover(const cover_art& cover)
{
    const std::string _hash = normalize_hash(cover.hash);
    const std::string _content_type = normalized_cover_type(cover.content_type);
    require_value(!cover.bytes.empty() && cover.bytes.size() <= maximum_cover_size, "Cover data is empty or too large");
    require_value(data_sha256(cover.bytes) == _hash, "Cover data does not match its hash");

    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _upsert(_implementation->_database, R"sql(
        INSERT INTO covers(hash, content_type, data)
        VALUES(?1, ?2, ?3)
        ON CONFLICT(hash) DO UPDATE SET
            content_type = excluded.content_type,
            data = excluded.data
    )sql");
    _upsert.bind(1, _hash);
    _upsert.bind(2, _content_type);
    _upsert.bind(3, cover.bytes);
    _upsert.execute();
}

void storage::register_external_file(const file_location& file)
{
    require_value(file.kind == file_kind::external, "External file must use external file kind");
    require_value(!file.path.empty(), "External file path cannot be empty");
    const std::string _normalized = normalize_hash(file.hash);
    const std::string _extension = normalized_extension(file.extension);

    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _upsert(_implementation->_database, R"sql(
        INSERT INTO files(hash, locator, extension, kind, size)
        VALUES(?1, ?2, ?3, 0, ?4)
        ON CONFLICT(hash) DO UPDATE SET
            locator = excluded.locator,
            extension = excluded.extension,
            size = excluded.size
        WHERE files.kind = 0
    )sql");
    _upsert.bind(1, _normalized);
    _upsert.bind(2, file.path);
    _upsert.bind(3, _extension);
    _upsert.bind(4, sqlite_integer(file.size_bytes, "File size"));
    _upsert.execute();
}

std::vector<track> storage::missing_files(std::string_view catalog_id) const
{
    require_value(!catalog_id.empty(), "Catalog ID cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, R"sql(
        SELECT t.id, t.catalog_id, t.file_hash, t.cover_hash,
               t.cover_content_type, t.cover_size, t.extension, t.title,
               t.artist, t.album, t.track_number, t.duration_ms, t.size
        FROM tracks AS t
        LEFT JOIN files AS f ON f.hash = t.file_hash
        WHERE t.catalog_id = ?1 AND f.hash IS NULL
        ORDER BY t.id
    )sql");
    _query.bind(1, catalog_id);

    std::vector<track> _result;
    while (_query.next()) {
        _result.push_back(read_track(_query));
    }
    return _result;
}

std::vector<track> storage::requested_offline_tracks() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, R"sql(
        SELECT DISTINCT t.id, t.catalog_id, t.file_hash, t.cover_hash,
               t.cover_content_type, t.cover_size, t.extension, t.title,
               t.artist, t.album, t.track_number, t.duration_ms, t.size
        FROM tracks AS t
        JOIN offline_tracks AS o
          ON o.catalog_id = t.catalog_id AND o.track_id = t.id
        ORDER BY t.catalog_id, t.id
    )sql");

    std::vector<track> _result;
    while (_query.next()) {
        _result.push_back(read_track(_query));
    }
    return _result;
}

bool storage::track_offline_requested(std::string_view catalog_id, std::string_view track_id) const
{
    require_value(!catalog_id.empty(), "Catalog ID cannot be empty");
    require_value(!track_id.empty(), "Track ID cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, R"sql(
        SELECT 1 FROM offline_tracks
        WHERE catalog_id = ?1 AND track_id = ?2
    )sql");
    _query.bind(1, catalog_id);
    _query.bind(2, track_id);
    return _query.next();
}

void storage::set_track_offline(std::string_view catalog_id, std::string_view track_id, bool offline)
{
    require_value(!catalog_id.empty(), "Catalog ID cannot be empty");
    require_value(!track_id.empty(), "Track ID cannot be empty");
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    if (offline) {
        statement _insert(_implementation->_database, R"sql(
            INSERT INTO offline_tracks(catalog_id, track_id)
            VALUES(?1, ?2)
            ON CONFLICT(catalog_id, track_id) DO NOTHING
        )sql");
        _insert.bind(1, catalog_id);
        _insert.bind(2, track_id);
        _insert.execute();
    } else {
        statement _remove(_implementation->_database, R"sql(
            DELETE FROM offline_tracks
            WHERE catalog_id = ?1 AND track_id = ?2
        )sql");
        _remove.bind(1, catalog_id);
        _remove.bind(2, track_id);
        _remove.execute();
    }
}

std::filesystem::path storage::partial_path(std::string_view hash) const
{
    const std::string _normalized = normalize_hash(hash);
    return _implementation->_asset_directory / ".partial" / (_normalized + ".part");
}

std::filesystem::path storage::managed_path(
    std::string_view hash,
    audio_extension extension) const
{
    const std::string _normalized = normalize_hash(hash);
    const std::string _file_name = _normalized + "." + normalized_extension(extension);
    return _implementation->_asset_directory / _normalized.substr(0, 2) / _file_name;
}

void storage::commit_download(
    std::string_view hash,
    audio_extension extension,
    std::uint64_t expected_size)
{
    const std::string _normalized = normalize_hash(hash);
    const std::string _extension_name = normalized_extension(extension);
    sqlite_integer(expected_size, "File size");
    const std::filesystem::path _partial = partial_path(_normalized);
    const std::filesystem::path _destination = managed_path(_normalized, extension);

    std::error_code _filesystem_error;
    require_value(std::filesystem::is_regular_file(_partial, _filesystem_error), "Partial download does not exist");
    if (_filesystem_error) {
        throw storage_error("Could not inspect partial download: " + _filesystem_error.message());
    }

    const std::uintmax_t _actual_size = std::filesystem::file_size(_partial, _filesystem_error);
    if (_filesystem_error) {
        throw storage_error("Could not read partial download size: " + _filesystem_error.message());
    }
    require_value(_actual_size == expected_size, "Downloaded file size does not match catalog");
    require_value(file_sha256(_partial) == _normalized, "Downloaded file hash does not match catalog");

    std::filesystem::create_directories(_destination.parent_path(), _filesystem_error);
    if (_filesystem_error) {
        throw storage_error("Could not create managed asset directory: " + _filesystem_error.message());
    }

    if (std::filesystem::exists(_destination, _filesystem_error)) {
        if (_filesystem_error) {
            throw storage_error("Could not inspect managed asset: " + _filesystem_error.message());
        }
        require_value(
            std::filesystem::file_size(_destination, _filesystem_error) == expected_size && !_filesystem_error && file_sha256(_destination) == _normalized,
            "Existing managed asset does not match its content hash");
        std::filesystem::remove(_partial, _filesystem_error);
        if (_filesystem_error) {
            throw storage_error("Could not remove redundant partial download: " + _filesystem_error.message());
        }
    } else {
        std::filesystem::rename(_partial, _destination, _filesystem_error);
        if (_filesystem_error) {
            throw storage_error("Could not finalize managed asset: " + _filesystem_error.message());
        }
    }

    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _upsert(_implementation->_database, R"sql(
        INSERT INTO files(hash, locator, extension, kind, size)
        VALUES(?1, ?2, ?3, 1, ?4)
        ON CONFLICT(hash) DO UPDATE SET
            locator = excluded.locator,
            extension = excluded.extension,
            kind = excluded.kind,
            size = excluded.size
    )sql");
    _upsert.bind(1, _normalized);
    _upsert.bind(2, path_string(_destination));
    _upsert.bind(3, _extension_name);
    _upsert.bind(4, sqlite_integer(expected_size, "File size"));
    _upsert.execute();
}

void storage::remove_managed_file(std::string_view hash)
{
    const std::string _normalized = normalize_hash(hash);
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    statement _query(_implementation->_database, "SELECT locator, kind FROM files WHERE hash = ?1");
    _query.bind(1, _normalized);
    if (!_query.next()) {
        return;
    }
    require_value(_query.integer(1) == static_cast<sqlite3_int64>(file_kind::managed), "External library files cannot be removed by offline storage");

    const std::filesystem::path _path = std::filesystem::u8path(_query.text(0));
    std::error_code _filesystem_error;
    const bool _removed = std::filesystem::remove(_path, _filesystem_error);
    if (_filesystem_error) {
        throw storage_error("Could not remove managed asset: " + _filesystem_error.message());
    }
    require_value(_removed || !std::filesystem::exists(_path), "Managed asset could not be removed");

    statement _remove(_implementation->_database, "DELETE FROM files WHERE hash = ?1 AND kind = 1");
    _remove.bind(1, _normalized);
    _remove.execute();
}

struct library_scanner::implementation {
    explicit implementation(storage& store)
        : _store(store)
    {
    }

    ~implementation()
    {
        if (_worker.joinable()) {
            _worker.join();
        }
    }

    storage& _store;
    mutable std::mutex _mutex;
    std::thread _worker;
    library_scan_status _current;
    bool _rescan_requested { false };
};

library_scanner::library_scanner(storage& store)
    : _implementation(std::make_unique<implementation>(store))
{
}

library_scanner::~library_scanner() = default;

void library_scanner::scan()
{
    std::thread _previous;
    {
        std::lock_guard<std::mutex> _lock(_implementation->_mutex);
        if (_implementation->_current.state == library_scan_state::scanning) {
            _implementation->_rescan_requested = true;
            return;
        }
        _previous = std::move(_implementation->_worker);
        _implementation->_current = { library_scan_state::scanning, { }, { } };
    }

    if (_previous.joinable()) {
        _previous.join();
    }

    try {
        _implementation->_worker = std::thread([_implementation = _implementation.get()] {
            for (;;) {
                library_scan_status _finished;
                try {
                    _finished = { library_scan_state::completed, _implementation->_store.scan_library(), { } };
                } catch (const std::exception& _exception) {
                    _finished = { library_scan_state::failed, { }, _exception.what() };
                } catch (...) {
                    _finished = { library_scan_state::failed, { }, "Unknown error while scanning the music library" };
                }

                std::lock_guard<std::mutex> _lock(_implementation->_mutex);
                if (!_implementation->_rescan_requested) {
                    _implementation->_current = std::move(_finished);
                    return;
                }
                _implementation->_rescan_requested = false;
            }
        });
    } catch (...) {
        std::lock_guard<std::mutex> _lock(_implementation->_mutex);
        _implementation->_current = { library_scan_state::failed, { }, "Could not start the music library scan" };
        throw;
    }
}

library_scan_status library_scanner::status() const
{
    std::lock_guard<std::mutex> _lock(_implementation->_mutex);
    return _implementation->_current;
}

}
