#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <core/cover.hpp>
#include <core/peer.hpp>
#include <core/security.hpp>
#include <core/storage.hpp>

namespace soundstep {

struct cover_cache::implementation {
    explicit implementation(storage& store)
        : _store(store)
        , _worker([this] { run(); })
    {
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

    std::optional<renderer_texture> texture(const track& track)
    {
        if (track.cover_hash.empty()) {
            return std::nullopt;
        }
        const std::unordered_map<std::string, renderer_texture>::const_iterator _cached = _textures.find(track.cover_hash);
        if (_cached != _textures.end()) {
            return _cached->second;
        }

        const std::optional<cover_art> _cover = _store.cover(track.cover_hash);
        if (!_cover) {
            request(track);
            return std::nullopt;
        }

        int _width = 0;
        int _height = 0;
        int _channels = 0;
        unsigned char* _pixels = stbi_load_from_memory(_cover->bytes.data(), static_cast<int>(_cover->bytes.size()), &_width, &_height, &_channels, 4);
        if (_pixels == nullptr || _width <= 0 || _height <= 0) {
            stbi_image_free(_pixels);
            throw cover_error("Could not decode embedded cover image");
        }

        renderer_texture _result;
        try {
            _result = renderer::create_rgba_texture(_pixels, _width, _height);
        } catch (...) {
            stbi_image_free(_pixels);
            throw;
        }
        stbi_image_free(_pixels);

        _textures.emplace(track.cover_hash, _result);
        return _result;
    }

    void request(const track& track)
    {
        std::lock_guard<std::mutex> _lock(_mutex);
        const std::unordered_map<std::string, std::chrono::steady_clock::time_point>::const_iterator _retry = _retry_after.find(track.cover_hash);
        if (_retry != _retry_after.end()
            && std::chrono::steady_clock::now() < _retry->second) {
            return;
        }
        if (!_requested.insert(track.cover_hash).second) {
            return;
        }
        _queue.push_back(track);
        _condition.notify_one();
    }

    bool fetch(const track& track)
    {
        const std::vector<peer_record> _peers = _store.peers();
        const peer_record* _owner = nullptr;
        for (const peer_record& _record : _peers) {
            if (_record.id == track.catalog_id) {
                _owner = &_record;
                break;
            }
        }
        if (_owner == nullptr || _owner->token.empty()
            || _owner->fingerprint.empty() || _owner->endpoints.empty()) {
            return false;
        }

        peer_client _client(_store, *_owner);
        const peer_response _response = _client.fetch_cover(track.cover_hash);
        if (!_response || _response.body.size() != track.cover_size_bytes
            || data_fingerprint(_response.body) != track.cover_hash) {
            return false;
        }
        const std::vector<unsigned char> _data(_response.body.begin(), _response.body.end());
        _store.store_cover({ track.cover_hash, track.cover_content_type, _data });
        return true;
    }

    void run() noexcept
    {
        for (;;) {
            track _value;
            {
                std::unique_lock<std::mutex> _lock(_mutex);
                _condition.wait(_lock, [this] { return _stop || !_queue.empty(); });
                if (_stop) {
                    return;
                }
                _value = std::move(_queue.front());
                _queue.pop_front();
            }
            bool _fetched = false;
            try {
                _fetched = fetch(_value);
            } catch (...) {
            }
            if (!_fetched) {
                std::lock_guard<std::mutex> _lock(_mutex);
                _requested.erase(_value.cover_hash);
                _retry_after.insert_or_assign(_value.cover_hash, std::chrono::steady_clock::now() + std::chrono::seconds(30));
            }
        }
    }

    void release_textures() noexcept
    {
        for (const std::pair<const std::string, renderer_texture>& _item : _textures) {
            renderer::destroy_texture(_item.second);
        }
        _textures.clear();
    }

    storage& _store;
    std::mutex _mutex;
    std::condition_variable _condition;
    std::deque<track> _queue;
    std::unordered_set<std::string> _requested;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> _retry_after;
    std::unordered_map<std::string, renderer_texture> _textures;
    bool _stop { false };
    std::thread _worker;
};

cover_cache::cover_cache(storage& store)
    : _implementation(std::make_unique<implementation>(store))
{
}

cover_cache::~cover_cache() = default;

std::optional<renderer_texture> cover_cache::texture(const track& track)
{
    try {
        return _implementation->texture(track);
    } catch (...) {
        return std::nullopt;
    }
}

void cover_cache::request(const track& track) noexcept
{
    try {
        _implementation->request(track);
    } catch (...) {
    }
}

void cover_cache::release_textures() noexcept
{
    _implementation->release_textures();
}

}
