#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <core/offline.hpp>
#include <core/peer.hpp>
#include <core/security.hpp>
#include <core/storage.hpp>

namespace soundstep {
namespace {

    struct _offline_entry {
        offline_state _state { offline_state::off };
        std::uint64_t _downloaded_bytes { 0 };
        std::uint64_t _total_bytes { 0 };
        std::uint64_t _request_id { 0 };
        bool _requested { false };
        std::string _error;
    };

    struct _offline_task {
        track _track;
        peer_record _peer;
        std::uint64_t _request_id { 0 };
    };

    std::string _peer_error_message(const peer_response& response)
    {
        if (!response.error_message.empty()) {
            return response.error_message;
        }
        if (response.status_code != 0) {
            return "HTTP " + std::to_string(response.status_code);
        }
        return "unknown peer error";
    }

}

struct offline_service::implementation {
    explicit implementation(storage& store)
        : _store(store)
        , _worker([this] { run(); })
    {
        reconcile();
    }

    ~implementation()
    {
        {
            std::lock_guard<std::mutex> _lock(_mutex);
            _stop = true;
        }
        _condition.notify_all();
        if (_worker.joinable()) {
            _worker.join();
        }
    }

    void save(const track& value)
    {
        const std::optional<file_location> _file = _store.find_file(value.file_hash);
        if (_file && _file->kind == file_kind::external) {
            return;
        }
        if (value.size_bytes == 0) {
            throw offline_error("Cannot save an empty audio asset offline");
        }

        const std::vector<peer_record> _peers = _store.peers();
        const peer_record* _owner = nullptr;
        for (const peer_record& _record : _peers) {
            if (_record.id == value.catalog_id) {
                _owner = &_record;
                break;
            }
        }
        if (_owner == nullptr) {
            throw offline_error("The catalog owner is not a paired instance");
        }
        if (_owner->token.empty()) {
            throw offline_error("Pair with this instance again before saving its tracks offline");
        }
        if (_owner->fingerprint.empty()) {
            throw offline_error("Pair with this instance again before connecting securely");
        }
        if (_owner->endpoints.empty()) {
            throw offline_error("This friend has no known network endpoint");
        }
        _store.set_track_offline(value.catalog_id, value.id, true);
        reconcile();
    }

    void remove(const track& value)
    {
        const std::optional<file_location> _file = _store.find_file(value.file_hash);
        if (_file && _file->kind == file_kind::external) {
            throw offline_error("Files from the local music folder cannot be removed here");
        }

        _store.set_track_offline(value.catalog_id, value.id, false);

        const std::vector<track> _remaining = _store.requested_offline_tracks();
        const std::vector<track>::const_iterator _shared = std::find_if(
            _remaining.begin(),
            _remaining.end(),
            [&value](const track& item) { return item.file_hash == value.file_hash; });
        if (_shared != _remaining.end()) {
            reconcile();
            return;
        }

        std::lock_guard<std::mutex> _lock(_mutex);
        const std::unordered_map<std::string, _offline_entry>::iterator _entry = _entries.find(value.file_hash);
        const bool _downloading = _entry != _entries.end()
            && _entry->second._state == offline_state::downloading;
        if (_entry != _entries.end()) {
            _entry->second._requested = false;
            _entry->second._state = offline_state::off;
            _entry->second._error.clear();
        }

        if (_file) {
            _store.remove_managed_file(value.file_hash);
        }

        if (_entry != _entries.end() && !_downloading) {
            _entries.erase(_entry);
            std::error_code _filesystem_error;
            std::filesystem::remove(_store.partial_path(value.file_hash), _filesystem_error);
        }
    }

    offline_status status(const track& value) const
    {
        const bool _requested = _store.track_offline_requested(value.catalog_id, value.id);
        const std::optional<file_location> _file = _store.find_file(value.file_hash);
        if (_file) {
            return {
                offline_state::on,
                _file->size_bytes,
                value.size_bytes,
                _file->kind == file_kind::managed,
                _requested,
                { }
            };
        }

        std::lock_guard<std::mutex> _lock(_mutex);
        const std::unordered_map<std::string, _offline_entry>::const_iterator _entry = _entries.find(value.file_hash);
        if (_entry == _entries.end() || !_entry->second._requested) {
            return { offline_state::off, 0, value.size_bytes, false, _requested, { } };
        }
        return {
            _entry->second._state,
            _entry->second._downloaded_bytes,
            _entry->second._total_bytes,
            false,
            _requested,
            _entry->second._error
        };
    }

    void reconcile()
    {
        const std::vector<track> _requested_tracks = _store.requested_offline_tracks();
        const std::vector<peer_record> _peers = _store.peers();
        std::unordered_set<std::string> _requested_hashes;
        _requested_hashes.reserve(_requested_tracks.size());

        {
            std::lock_guard<std::mutex> _lock(_mutex);
            for (const track& _track : _requested_tracks) {
                _requested_hashes.insert(_track.file_hash);
                const bool _audio_available = _store.find_file(_track.file_hash).has_value();
                const bool _cover_available = _track.cover_hash.empty()
                    || _store.cover(_track.cover_hash).has_value();
                const std::unordered_map<std::string, _offline_entry>::iterator _existing = _entries.find(_track.file_hash);
                if (_audio_available && _cover_available) {
                    _entries.insert_or_assign(_track.file_hash, _offline_entry { offline_state::on, _track.size_bytes, _track.size_bytes, _existing == _entries.end() ? 0 : _existing->second._request_id, true, { } });
                    continue;
                }
                if (_existing != _entries.end()
                    && _existing->second._requested
                    && _existing->second._state == offline_state::downloading) {
                    continue;
                }

                const peer_record* _owner = nullptr;
                for (const peer_record& _record : _peers) {
                    if (_record.id == _track.catalog_id) {
                        _owner = &_record;
                        break;
                    }
                }
                if (_owner == nullptr || _owner->token.empty()
                    || _owner->fingerprint.empty() || _owner->endpoints.empty()) {
                    _entries.insert_or_assign(_track.file_hash, _offline_entry { offline_state::failed, 0, _track.size_bytes, 0, true, "The catalog owner is not currently reachable" });
                    continue;
                }

                const std::uint64_t _request_id = ++_next_request_id;
                _entries.insert_or_assign(_track.file_hash, _offline_entry { offline_state::downloading, 0, _track.size_bytes, _request_id, true, { } });
                _queue.push_back({ _track, *_owner, _request_id });
            }

            for (std::unordered_map<std::string, _offline_entry>::iterator _entry = _entries.begin();
                _entry != _entries.end();) {
                if (_requested_hashes.find(_entry->first) != _requested_hashes.end()) {
                    ++_entry;
                    continue;
                }
                if (_entry->second._state == offline_state::downloading) {
                    _entry->second._requested = false;
                    ++_entry;
                } else {
                    _entry = _entries.erase(_entry);
                }
            }
        }

        for (const file_location& _file : _store.managed_files()) {
            if (_requested_hashes.find(_file.hash) == _requested_hashes.end()) {
                _store.remove_managed_file(_file.hash);
            }
        }
        _condition.notify_one();
    }

    bool requested(std::string_view hash, std::uint64_t request_id) const
    {
        std::lock_guard<std::mutex> _lock(_mutex);
        const std::unordered_map<std::string, _offline_entry>::const_iterator _entry = _entries.find(std::string(hash));
        return !_stop
            && _entry != _entries.end()
            && _entry->second._request_id == request_id
            && _entry->second._requested;
    }

    void update_progress(
        std::string_view hash,
        std::uint64_t request_id,
        std::uint64_t downloaded_bytes)
    {
        std::lock_guard<std::mutex> _lock(_mutex);
        const std::unordered_map<std::string, _offline_entry>::iterator _entry = _entries.find(std::string(hash));
        if (_entry != _entries.end() && _entry->second._request_id == request_id) {
            _entry->second._downloaded_bytes = downloaded_bytes;
        }
    }

    void fail(const _offline_task& task, std::string message)
    {
        std::lock_guard<std::mutex> _lock(_mutex);
        const std::unordered_map<std::string, _offline_entry>::iterator _entry = _entries.find(task._track.file_hash);
        if (_entry == _entries.end() || _entry->second._request_id != task._request_id) {
            return;
        }
        if (!_entry->second._requested || _stop) {
            return;
        }
        _entry->second._state = offline_state::failed;
        _entry->second._error = std::move(message);
    }

    void cancel(const _offline_task& task, bool remove_partial)
    {
        bool _current_cancelled_request = false;
        {
            std::lock_guard<std::mutex> _lock(_mutex);
            const std::unordered_map<std::string, _offline_entry>::iterator _entry = _entries.find(task._track.file_hash);
            if (_entry != _entries.end()
                && _entry->second._request_id == task._request_id
                && !_entry->second._requested) {
                _entries.erase(_entry);
                _current_cancelled_request = true;
            }
        }

        if (_current_cancelled_request && remove_partial) {
            std::error_code _filesystem_error;
            std::filesystem::remove(_store.partial_path(task._track.file_hash), _filesystem_error);
        }
    }

    void download(const _offline_task& task)
    {
        const std::filesystem::path _partial = _store.partial_path(task._track.file_hash);
        std::error_code _filesystem_error;
        std::uint64_t _offset = std::filesystem::exists(_partial, _filesystem_error)
            ? std::filesystem::file_size(_partial, _filesystem_error)
            : 0;
        if (_filesystem_error) {
            throw offline_error("Could not inspect partial download: " + _filesystem_error.message());
        }
        if (_offset > task._track.size_bytes) {
            std::filesystem::remove(_partial, _filesystem_error);
            if (_filesystem_error) {
                throw offline_error("Could not discard invalid partial download: " + _filesystem_error.message());
            }
            _offset = 0;
        }
        update_progress(task._track.file_hash, task._request_id, _offset);

        peer_client _client(_store, task._peer);
        if (_offset < task._track.size_bytes) {
            std::ofstream _output(
                _partial,
                std::ios::binary | (_offset == 0 ? std::ios::trunc : std::ios::app));
            if (!_output) {
                throw offline_error("Could not open partial download for writing");
            }

            std::uint64_t _downloaded_bytes = _offset;
            peer_response _response = _client.stream_asset(
                task._track.file_hash,
                [this, &task, &_output, &_downloaded_bytes](const char* data, std::size_t size) {
                    if (!requested(task._track.file_hash, task._request_id)) {
                        return false;
                    }
                    _output.write(data, static_cast<std::streamsize>(size));
                    if (!_output) {
                        return false;
                    }
                    _downloaded_bytes += size;
                    update_progress(task._track.file_hash, task._request_id, _downloaded_bytes);
                    return true;
                },
                _offset,
                task._track.size_bytes - _offset);
            _output.flush();
            if (!_output && requested(task._track.file_hash, task._request_id)) {
                throw offline_error("Could not write partial download");
            }
            _output.close();

            if (!requested(task._track.file_hash, task._request_id)) {
                cancel(task, true);
                return;
            }
            if (!_response) {
                throw offline_error("Could not download audio asset: " + _peer_error_message(_response));
            }
        }

        if (!task._track.cover_hash.empty()
            && !_store.cover(task._track.cover_hash)) {
            const peer_response _cover_response = _client.fetch_cover(task._track.cover_hash);
            if (!_cover_response
                || _cover_response.body.size() != task._track.cover_size_bytes
                || data_fingerprint(_cover_response.body) != task._track.cover_hash) {
                throw offline_error("Could not download the track cover");
            }
            const std::vector<unsigned char> _cover_data(
                _cover_response.body.begin(),
                _cover_response.body.end());
            _store.store_cover({ task._track.cover_hash,
                task._track.cover_content_type,
                _cover_data });
        }

        bool _remove_cancelled_partial = false;
        {
            std::lock_guard<std::mutex> _lock(_mutex);
            const std::unordered_map<std::string, _offline_entry>::iterator _entry = _entries.find(task._track.file_hash);
            if (_entry == _entries.end() || _entry->second._request_id != task._request_id) {
                return;
            }
            if (!_entry->second._requested) {
                _entries.erase(_entry);
                _remove_cancelled_partial = true;
            } else if (_stop) {
                return;
            } else {
                if (!_store.find_file(task._track.file_hash)) {
                    _store.commit_download(
                        task._track.file_hash,
                        task._track.extension,
                        task._track.size_bytes);
                }
                _entry->second._state = offline_state::on;
                _entry->second._downloaded_bytes = task._track.size_bytes;
                _entry->second._error.clear();
            }
        }

        if (_remove_cancelled_partial) {
            std::filesystem::remove(_partial, _filesystem_error);
        }
    }

    void run() noexcept
    {
        while (true) {
            _offline_task _task;
            {
                std::unique_lock<std::mutex> _lock(_mutex);
                _condition.wait_for(
                    _lock,
                    std::chrono::seconds(30),
                    [this] { return _stop || !_queue.empty(); });
                if (_stop) {
                    return;
                }
                if (_queue.empty()) {
                    _lock.unlock();
                    try {
                        reconcile();
                    } catch (...) {
                    }
                    continue;
                }
                _task = std::move(_queue.front());
                _queue.pop_front();
            }

            if (!requested(_task._track.file_hash, _task._request_id)) {
                cancel(_task, true);
                continue;
            }
            try {
                download(_task);
            } catch (const std::exception& _exception) {
                fail(_task, _exception.what());
            } catch (...) {
                fail(_task, "Unknown offline download error");
            }
        }
    }

    storage& _store;
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    std::deque<_offline_task> _queue;
    std::unordered_map<std::string, _offline_entry> _entries;
    std::uint64_t _next_request_id { 0 };
    bool _stop { false };
    std::thread _worker;
};

offline_service::offline_service(storage& store)
    : _implementation(std::make_unique<implementation>(store))
{
}

offline_service::~offline_service() = default;

void offline_service::save(const track& track)
{
    _implementation->save(track);
}

void offline_service::remove(const track& track)
{
    _implementation->remove(track);
}

void offline_service::reconcile()
{
    _implementation->reconcile();
}

offline_status offline_service::status(const track& track) const
{
    return _implementation->status(track);
}

}
