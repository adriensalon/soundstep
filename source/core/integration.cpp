#include <core/context.hpp>
#include <core/integration.hpp>

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <SystemMediaTransportControlsInterop.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <widget/library.hpp>

namespace soundstep {
namespace {

    namespace _media = winrt::Windows::Media;
    namespace _foundation = winrt::Windows::Foundation;
    namespace _storage = winrt::Windows::Storage;
    namespace _streams = winrt::Windows::Storage::Streams;

    enum struct _transport_request_kind {
        play,
        pause,
        stop,
        previous,
        next,
        seek
    };

    struct _transport_request {
        _transport_request_kind kind;
        double position_seconds { 0.0 };
    };

    _foundation::TimeSpan _time_span(double seconds)
    {
        const double _safe_seconds = std::isfinite(seconds) ? (std::max)(0.0, seconds) : 0.0;
        return std::chrono::duration_cast<_foundation::TimeSpan>(std::chrono::duration<double>(_safe_seconds));
    }

    _media::MediaPlaybackStatus _media_status(playback_state state)
    {
        switch (state) {
        case playback_state::buffering:
            return _media::MediaPlaybackStatus::Changing;
        case playback_state::playing:
            return _media::MediaPlaybackStatus::Playing;
        case playback_state::paused:
            return _media::MediaPlaybackStatus::Paused;
        case playback_state::stopped:
        case playback_state::finished:
            return _media::MediaPlaybackStatus::Stopped;
        case playback_state::failed:
            return _media::MediaPlaybackStatus::Closed;
        }
        return _media::MediaPlaybackStatus::Closed;
    }

    struct _cover_thumbnail {
        _storage::StorageFile file { nullptr };
        _streams::RandomAccessStreamReference reference { nullptr };
    };

    _cover_thumbnail _create_cover_thumbnail(const cover_art& cover, std::filesystem::path& directory)
    {
        if (directory.empty()) {
            directory = std::filesystem::temp_directory_path()
                / ("SoundStep-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
            std::filesystem::create_directories(directory);
        }

        const std::string _extension = cover.content_type == "image/png" ? ".png" : ".jpg";
        const std::filesystem::path _path = directory / (cover.hash + _extension);
        std::ofstream _output(_path, std::ios::binary | std::ios::trunc);
        if (!_output) {
            throw std::runtime_error("Could not create the Windows media thumbnail file");
        }
        _output.write(reinterpret_cast<const char*>(cover.bytes.data()), static_cast<std::streamsize>(cover.bytes.size()));
        _output.close();
        if (!_output) {
            throw std::runtime_error("Could not write the Windows media thumbnail file");
        }

        _cover_thumbnail _result;
        _result.file = _storage::StorageFile::GetFileFromPathAsync(_path.wstring()).get();
        _result.reference = _streams::RandomAccessStreamReference::CreateFromFile(_result.file);
        return _result;
    }

}

struct system_media_transport::implementation {
    explicit implementation(void* native_window)
    {
        initialize(static_cast<HWND>(native_window));
    }

    ~implementation()
    {
        shutdown();
    }

    void initialize(HWND window) noexcept
    {
        if (window == nullptr) {
            return;
        }

        try {
            const HRESULT _initialize_result = RoInitialize(RO_INIT_MULTITHREADED);
            if (SUCCEEDED(_initialize_result)) {
                _apartment_initialized = true;
            } else if (_initialize_result != RPC_E_CHANGED_MODE) {
                return;
            }

            const auto _interop = winrt::get_activation_factory<_media::SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
            winrt::check_hresult(_interop->GetForWindow(window, winrt::guid_of<_media::SystemMediaTransportControls>(), winrt::put_abi(_controls)));

            _button_token = _controls.ButtonPressed(
                [this](const _media::SystemMediaTransportControls&,
                    const _media::SystemMediaTransportControlsButtonPressedEventArgs& args) {
                    switch (args.Button()) {
                    case _media::SystemMediaTransportControlsButton::Play:
                        enqueue({ _transport_request_kind::play });
                        break;
                    case _media::SystemMediaTransportControlsButton::Pause:
                        enqueue({ _transport_request_kind::pause });
                        break;
                    case _media::SystemMediaTransportControlsButton::Stop:
                        enqueue({ _transport_request_kind::stop });
                        break;
                    case _media::SystemMediaTransportControlsButton::Previous:
                        enqueue({ _transport_request_kind::previous });
                        break;
                    case _media::SystemMediaTransportControlsButton::Next:
                        enqueue({ _transport_request_kind::next });
                        break;
                    default:
                        break;
                    }
                });
            _button_handler_registered = true;

            _position_token = _controls.PlaybackPositionChangeRequested([this](const _media::SystemMediaTransportControls&, const _media::PlaybackPositionChangeRequestedEventArgs& args) {
                enqueue({ _transport_request_kind::seek, std::chrono::duration<double>(args.RequestedPlaybackPosition()).count() });
            });
            _position_handler_registered = true;

            _controls.IsPlayEnabled(true);
            _controls.IsPauseEnabled(true);
            _controls.IsStopEnabled(true);
            _controls.IsFastForwardEnabled(false);
            _controls.IsRewindEnabled(false);
            _controls.IsEnabled(false);
        } catch (...) {
            shutdown();
        }
    }

    void shutdown() noexcept
    {
        if (_controls) {
            try {
                if (_position_handler_registered) {
                    _controls.PlaybackPositionChangeRequested(_position_token);
                }
                if (_button_handler_registered) {
                    _controls.ButtonPressed(_button_token);
                }
                _controls.IsEnabled(false);
            } catch (...) {
            }
        }
        _position_handler_registered = false;
        _button_handler_registered = false;
        _thumbnail_reference = nullptr;
        _thumbnail_file = nullptr;
        _controls = nullptr;

        if (!_thumbnail_directory.empty()) {
            std::error_code _filesystem_error;
            std::filesystem::remove_all(_thumbnail_directory, _filesystem_error);
            _thumbnail_directory.clear();
        }

        if (_apartment_initialized) {
            RoUninitialize();
            _apartment_initialized = false;
        }
    }

    void enqueue(_transport_request request) noexcept
    {
        try {
            std::lock_guard<std::mutex> _lock(_request_mutex);
            _requests.push_back(request);
        } catch (...) {
        }
    }

    void apply_requests(context& ctx)
    {
        std::deque<_transport_request> _pending;
        {
            std::lock_guard<std::mutex> _lock(_request_mutex);
            _pending.swap(_requests);
        }

        for (const _transport_request& _request : _pending) {
            switch (_request.kind) {
            case _transport_request_kind::play:
                ctx.try_action([&ctx] { ctx.player.play(); });
                break;
            case _transport_request_kind::pause:
                ctx.try_action([&ctx] { ctx.player.pause(); });
                break;
            case _transport_request_kind::stop:
                ctx.try_action([&ctx] { ctx.player.stop(); });
                break;
            case _transport_request_kind::previous:
                if (can_play_previous_track(ctx)) {
                    play_previous_track(ctx);
                }
                break;
            case _transport_request_kind::next:
                if (can_play_next_track(ctx)) {
                    play_next_track(ctx);
                }
                break;
            case _transport_request_kind::seek: {
                const playback_status _status = ctx.player.status();
                if (_status.has_source) {
                    double _position = (std::max)(0.0, _request.position_seconds);
                    const double _duration = ctx.current_track && ctx.current_track->duration_ms != 0
                        ? static_cast<double>(ctx.current_track->duration_ms) / 1000.0
                        : _status.duration_seconds;
                    if (_duration > 0.0) {
                        _position = (std::min)(_position, _duration);
                    }
                    ctx.try_action([&ctx, _position] { ctx.player.seek(_position); });
                }
                break;
            }
            }
        }
    }

    void update_metadata(context& ctx)
    {
        std::string _identity;
        if (ctx.current_track) {
            _identity = ctx.current_track->catalog_id + "\n" + ctx.current_track->id + "\n"
                + ctx.current_track->title + "\n" + ctx.current_track->artist + "\n"
                + ctx.current_track->album + "\n" + std::to_string(ctx.current_track->track_number) + "\n"
                + ctx.current_track->cover_hash;
        }
        const bool _metadata_changed = _identity != _metadata_identity;
        const std::string _desired_cover = ctx.current_track ? ctx.current_track->cover_hash : std::string { };
        const std::chrono::steady_clock::time_point _now = std::chrono::steady_clock::now();
        const bool _cover_pending = !_desired_cover.empty()
            && _thumbnail_hash != _desired_cover
            && _now >= _next_thumbnail_update;
        if (!_metadata_changed && !_cover_pending) {
            return;
        }
        if (_metadata_changed) {
            _metadata_identity = std::move(_identity);
            _thumbnail_hash.clear();
            _thumbnail_reference = nullptr;
            _thumbnail_file = nullptr;
            _next_thumbnail_update = { };
        }

        _media::SystemMediaTransportControlsDisplayUpdater _updater = _controls.DisplayUpdater();
        _updater.ClearAll();
        if (ctx.current_track) {
            _updater.Type(_media::MediaPlaybackType::Music);
            _media::MusicDisplayProperties _properties = _updater.MusicProperties();
            _properties.Title(winrt::to_hstring(ctx.current_track->title.empty() ? std::string("Unknown") : ctx.current_track->title));
            _properties.Artist(winrt::to_hstring(ctx.current_track->artist));
            _properties.AlbumArtist(winrt::to_hstring(ctx.current_track->artist));
            _properties.AlbumTitle(winrt::to_hstring(ctx.current_track->album));
            _properties.TrackNumber(ctx.current_track->track_number);

            if (!ctx.current_track->cover_hash.empty()) {
                try {
                    const std::optional<cover_art> _cover = ctx.store.cover(ctx.current_track->cover_hash);
                    if (_cover) {
                        _cover_thumbnail _thumbnail = _create_cover_thumbnail(*_cover, _thumbnail_directory);
                        _thumbnail_file = std::move(_thumbnail.file);
                        _thumbnail_reference = std::move(_thumbnail.reference);
                        _updater.Thumbnail(_thumbnail_reference);
                        _thumbnail_hash = ctx.current_track->cover_hash;
                    } else {
                        ctx.covers.request(*ctx.current_track);
                        _next_thumbnail_update = _now + std::chrono::seconds(1);
                    }
                } catch (...) {
                    _next_thumbnail_update = _now + std::chrono::seconds(1);
                }
            }
        }
        _updater.Update();
    }

    void update_timeline(const context& ctx, const playback_status& status, bool force)
    {
        const std::chrono::steady_clock::time_point _now = std::chrono::steady_clock::now();
        if (!force && _now < _next_timeline_update) {
            return;
        }
        _next_timeline_update = _now + std::chrono::milliseconds(500);

        const double _duration = ctx.current_track && ctx.current_track->duration_ms != 0
            ? static_cast<double>(ctx.current_track->duration_ms) / 1000.0
            : status.duration_seconds;
        const double _position = _duration > 0.0
            ? (std::clamp)(status.position_seconds, 0.0, _duration)
            : 0.0;

        _media::SystemMediaTransportControlsTimelineProperties _timeline;
        _timeline.StartTime(_time_span(0.0));
        _timeline.MinSeekTime(_time_span(0.0));
        _timeline.EndTime(_time_span(_duration));
        _timeline.MaxSeekTime(_time_span(_duration));
        _timeline.Position(_time_span(_position));
        _controls.UpdateTimelineProperties(_timeline);
    }

    void update(context& ctx) noexcept
    {
        if (!_controls) {
            return;
        }

        try {
            apply_requests(ctx);
            const playback_status _status = ctx.player.status();
            const bool _has_source = _status.has_source;
            const bool _can_previous = can_play_previous_track(ctx);
            const bool _can_next = can_play_next_track(ctx);
            const _media::MediaPlaybackStatus _new_status = _media_status(_status.state);
            const bool _is_active = _status.state == playback_state::playing || _status.state == playback_state::buffering;
            const bool _can_stop = _has_source
                && _status.state != playback_state::stopped
                && _status.state != playback_state::finished;

            _controls.IsEnabled(_has_source);
            _controls.IsPlayEnabled(_has_source && !_is_active);
            _controls.IsPauseEnabled(_has_source && _is_active);
            _controls.IsStopEnabled(_can_stop);
            _controls.IsPreviousEnabled(_can_previous);
            _controls.IsNextEnabled(_can_next);
            _controls.PlaybackStatus(_new_status);
            update_metadata(ctx);

            const bool _force_timeline = !_has_last_status
                || _last_has_source != _has_source
                || _last_status != _new_status;
            update_timeline(ctx, _status, _force_timeline);
            _last_has_source = _has_source;
            _last_status = _new_status;
            _has_last_status = true;
        } catch (...) {
            shutdown();
        }
    }

    _media::SystemMediaTransportControls _controls { nullptr };
    winrt::event_token _button_token { };
    winrt::event_token _position_token { };
    bool _button_handler_registered { false };
    bool _position_handler_registered { false };
    bool _apartment_initialized { false };

    std::mutex _request_mutex;
    std::deque<_transport_request> _requests;
    std::string _metadata_identity;
    std::string _thumbnail_hash;
    std::filesystem::path _thumbnail_directory;
    _storage::StorageFile _thumbnail_file { nullptr };
    _streams::RandomAccessStreamReference _thumbnail_reference { nullptr };
    std::chrono::steady_clock::time_point _next_thumbnail_update { };
    std::chrono::steady_clock::time_point _next_timeline_update { };
    _media::MediaPlaybackStatus _last_status { _media::MediaPlaybackStatus::Closed };
    bool _last_has_source { false };
    bool _has_last_status { false };
};

system_media_transport::system_media_transport(void* native_window)
    : _implementation(std::make_unique<implementation>(native_window))
{
}

system_media_transport::~system_media_transport() = default;

void system_media_transport::update(context& ctx) noexcept
{
    _implementation->update(ctx);
}

}

#else

namespace soundstep {

struct system_media_transport::implementation {
};

system_media_transport::system_media_transport(void*)
    : _implementation(std::make_unique<implementation>())
{
}

system_media_transport::~system_media_transport() = default;

void system_media_transport::update(context&) noexcept
{
}

}

#endif
