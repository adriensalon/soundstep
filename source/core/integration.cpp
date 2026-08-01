#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include <SystemMediaTransportControlsInterop.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/base.h>

#include <core/integration.hpp>
#include <core/context.hpp>
#include <widget/library.hpp>

namespace soundstep {
namespace {

    namespace media = winrt::Windows::Media;

    enum struct transport_request_kind {
        play,
        pause,
        stop,
        previous,
        next,
        seek
    };

    struct transport_request {
        transport_request_kind kind;
        double position_seconds { 0.0 };
    };

    winrt::Windows::Foundation::TimeSpan _time_span(double seconds)
    {
        const double _safe_seconds = std::isfinite(seconds) ? (std::max)(0.0, seconds) : 0.0;
        return std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
            std::chrono::duration<double>(_safe_seconds));
    }

    media::MediaPlaybackStatus _media_status(playback_state state)
    {
        switch (state) {
        case playback_state::buffering:
            return media::MediaPlaybackStatus::Changing;
        case playback_state::playing:
            return media::MediaPlaybackStatus::Playing;
        case playback_state::paused:
            return media::MediaPlaybackStatus::Paused;
        case playback_state::stopped:
        case playback_state::finished:
            return media::MediaPlaybackStatus::Stopped;
        case playback_state::failed:
            return media::MediaPlaybackStatus::Closed;
        }
        return media::MediaPlaybackStatus::Closed;
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

            const auto _interop = winrt::get_activation_factory<
                media::SystemMediaTransportControls,
                ISystemMediaTransportControlsInterop>();
            winrt::check_hresult(_interop->GetForWindow(
                window,
                winrt::guid_of<media::SystemMediaTransportControls>(),
                winrt::put_abi(_controls)));

            _button_token = _controls.ButtonPressed(
                [this](const media::SystemMediaTransportControls&,
                    const media::SystemMediaTransportControlsButtonPressedEventArgs& args) {
                    switch (args.Button()) {
                    case media::SystemMediaTransportControlsButton::Play:
                        enqueue({ transport_request_kind::play });
                        break;
                    case media::SystemMediaTransportControlsButton::Pause:
                        enqueue({ transport_request_kind::pause });
                        break;
                    case media::SystemMediaTransportControlsButton::Stop:
                        enqueue({ transport_request_kind::stop });
                        break;
                    case media::SystemMediaTransportControlsButton::Previous:
                        enqueue({ transport_request_kind::previous });
                        break;
                    case media::SystemMediaTransportControlsButton::Next:
                        enqueue({ transport_request_kind::next });
                        break;
                    default:
                        break;
                    }
                });
            _button_handler_registered = true;

            _position_token = _controls.PlaybackPositionChangeRequested(
                [this](const media::SystemMediaTransportControls&,
                    const media::PlaybackPositionChangeRequestedEventArgs& args) {
                    enqueue({ transport_request_kind::seek,
                        std::chrono::duration<double>(args.RequestedPlaybackPosition()).count() });
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
        _controls = nullptr;

        if (_apartment_initialized) {
            RoUninitialize();
            _apartment_initialized = false;
        }
    }

    void enqueue(transport_request request) noexcept
    {
        try {
            std::lock_guard<std::mutex> _lock(_request_mutex);
            _requests.push_back(request);
        } catch (...) {
        }
    }

    void apply_requests(context& ctx)
    {
        std::deque<transport_request> _pending;
        {
            std::lock_guard<std::mutex> _lock(_request_mutex);
            _pending.swap(_requests);
        }

        for (const transport_request& _request : _pending) {
            switch (_request.kind) {
            case transport_request_kind::play:
                ctx.try_action([&ctx] { ctx.player.play(); });
                break;
            case transport_request_kind::pause:
                ctx.try_action([&ctx] { ctx.player.pause(); });
                break;
            case transport_request_kind::stop:
                ctx.try_action([&ctx] { ctx.player.stop(); });
                break;
            case transport_request_kind::previous:
                if (can_play_previous_track(ctx)) {
                    play_previous_track(ctx);
                }
                break;
            case transport_request_kind::next:
                if (can_play_next_track(ctx)) {
                    play_next_track(ctx);
                }
                break;
            case transport_request_kind::seek: {
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

    void update_metadata(const context& ctx)
    {
        std::string _identity;
        if (ctx.current_track) {
            _identity = ctx.current_track->catalog_id + "\n" + ctx.current_track->id + "\n"
                + ctx.current_track->title + "\n" + ctx.current_track->artist + "\n"
                + ctx.current_track->album + "\n" + std::to_string(ctx.current_track->track_number);
        }
        if (_identity == _metadata_identity) {
            return;
        }
        _metadata_identity = std::move(_identity);

        media::SystemMediaTransportControlsDisplayUpdater _updater = _controls.DisplayUpdater();
        _updater.ClearAll();
        if (ctx.current_track) {
            _updater.Type(media::MediaPlaybackType::Music);
            media::MusicDisplayProperties _properties = _updater.MusicProperties();
            _properties.Title(winrt::to_hstring(
                ctx.current_track->title.empty() ? std::string("Unknown") : ctx.current_track->title));
            _properties.Artist(winrt::to_hstring(ctx.current_track->artist));
            _properties.AlbumArtist(winrt::to_hstring(ctx.current_track->artist));
            _properties.AlbumTitle(winrt::to_hstring(ctx.current_track->album));
            _properties.TrackNumber(ctx.current_track->track_number);
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

        media::SystemMediaTransportControlsTimelineProperties _timeline;
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
            const media::MediaPlaybackStatus _new_status = _media_status(_status.state);
            const bool _is_active = _status.state == playback_state::playing
                || _status.state == playback_state::buffering;
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

    media::SystemMediaTransportControls _controls { nullptr };
    winrt::event_token _button_token { };
    winrt::event_token _position_token { };
    bool _button_handler_registered { false };
    bool _position_handler_registered { false };
    bool _apartment_initialized { false };

    std::mutex _request_mutex;
    std::deque<transport_request> _requests;
    std::string _metadata_identity;
    std::chrono::steady_clock::time_point _next_timeline_update { };
    media::MediaPlaybackStatus _last_status { media::MediaPlaybackStatus::Closed };
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
