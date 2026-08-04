#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <im_anim.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <core/context.hpp>
#include <view/animation.hpp>
#include <view/friends.hpp>
#include <view/icons.hpp>
#include <view/library.hpp>
#include <view/settings.hpp>

namespace soundstep {
namespace {

    enum struct _search_field {
        title,
        artist,
        album
    };

    struct _library_view_state {
        std::string _revision_key;
        std::unordered_map<std::string, std::string> _catalog_names;
        std::vector<track> _tracks;
        std::vector<std::size_t> _visible_track_indices;
        std::string _selected_catalog_id;
        std::string _selected_track_id;
        std::string _search;
        std::unordered_map<std::string, offline_state> _offline_states;
        _search_field _field { _search_field::title };
        library_scan_state _scan_state { library_scan_state::idle };
        bool _scan_state_initialized { false };
        bool _offline_only { false };
        bool _tracks_dirty { true };
        bool _selection_dirty { true };
#ifdef __ANDROID__
        bool _touch_scroll_active { false };
        bool _touch_scroll_moved { false };
        float _touch_scroll_start_y { 0.0f };
        float _touch_scroll_previous_y { 0.0f };
#endif
    };

    _library_view_state _view;

    struct _track_editor_state {
        std::string _title;
        std::string _artist;
        std::string _album;
        std::uint32_t _track_number { 0 };
    };

    _track_editor_state _editor;

    struct _message_visual_state {
        std::string _text;
        widget_message_kind _kind { widget_message_kind::progress };
    };

    _message_visual_state _message_visual;

    constexpr float _library_list_horizontal_padding = 12.0f;
    constexpr float _track_card_padding = 8.0f;
    constexpr float _cover_text_spacing = 12.0f;
    constexpr float _card_right_extension = 4.0f;

    void _draw_offline_toggle(bool& active)
    {
        const ImGuiStyle& _style = ImGui::GetStyle();
        const ImGuiID _owner = ImGui::GetID("##OfflineFilterMotion");
        const ImVec2 _position = ImGui::GetCursorScreenPos();
        const ImVec2 _size(ImGui::CalcTextSize(icons::offline_only).x + _style.FramePadding.x * 2.0f, ImGui::GetFrameHeight());
        const bool _hovered = ImGui::IsMouseHoveringRect(_position, ImVec2(_position.x + _size.x, _position.y + _size.y));
        const float _state = animation_tween(_owner, 0x11001u, active ? 1.0f : 0.0f, animation_quick, iam_ease_out_cubic);
        const float _hover = animation_tween(_owner, 0x11002u, _hovered ? 1.0f : 0.0f, animation_quick, iam_ease_out_cubic);
        const ImVec4 _resting = iam_get_blended_color(_style.Colors[ImGuiCol_Button], _style.Colors[ImGuiCol_HeaderActive], _state, iam_col_oklab);
        const ImVec4 _hovered_color = iam_get_blended_color(_style.Colors[ImGuiCol_ButtonHovered], _style.Colors[ImGuiCol_SliderGrab], _state, iam_col_oklab);
        const ImVec4 _color = iam_get_blended_color(_resting, _hovered_color, _hover, iam_col_oklab);
        ImGui::PushStyleColor(ImGuiCol_Button, _color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, _color);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? _style.Colors[ImGuiCol_SliderGrabActive] : _style.Colors[ImGuiCol_ButtonActive]);
        if (ImGui::Button(icons::offline_only)) {
            active = !active;
        }
        ImGui::PopStyleColor(3);
    }

    using _track_identity = std::tuple<
        std::string,
        std::string,
        std::string,
        std::uint32_t,
        std::uint64_t>;

    std::string _normalized_identity_text(std::string_view text)
    {
        std::string _normalized;
        _normalized.reserve(text.size());
        bool _pending_space = false;
        for (const unsigned char _character : text) {
            if (std::isspace(_character)) {
                _pending_space = !_normalized.empty();
                continue;
            }
            if (_pending_space) {
                _normalized.push_back(' ');
                _pending_space = false;
            }
            _normalized.push_back(static_cast<char>(std::tolower(_character)));
        }
        return _normalized;
    }

    std::uint64_t _duration_bucket(std::uint64_t duration_ms)
    {
        constexpr std::uint64_t _bucket_size_ms = 2'000;
        const std::uint64_t _bucket = duration_ms / _bucket_size_ms;
        return duration_ms % _bucket_size_ms >= _bucket_size_ms / 2 ? _bucket + 1 : _bucket;
    }

    _track_identity _identity(const track& value)
    {
        std::string _title = _normalized_identity_text(value.title);
        if (_title.empty()) {
            _title = value.catalog_id + ":" + value.id;
        }
        return {
            std::move(_title),
            _normalized_identity_text(value.artist),
            _normalized_identity_text(value.album),
            value.track_number,
            _duration_bucket(value.duration_ms)
        };
    }

    bool _same_recording(const track& left, const track& right)
    {
        return _identity(left) == _identity(right);
    }

    bool _prefer_variant(const track& candidate, const track& current, std::string_view local_catalog_id)
    {
        const int _candidate_priority = audio_extension_priority(candidate.extension);
        const int _current_priority = audio_extension_priority(current.extension);
        if (_candidate_priority != _current_priority) {
            return _candidate_priority > _current_priority;
        }

        const bool _candidate_local = candidate.catalog_id == local_catalog_id;
        const bool _current_local = current.catalog_id == local_catalog_id;
        if (_candidate_local != _current_local) {
            return _candidate_local;
        }
        if (candidate.size_bytes != current.size_bytes) {
            return candidate.size_bytes > current.size_bytes;
        }
        return std::tie(candidate.catalog_id, candidate.id) < std::tie(current.catalog_id, current.id);
    }

    void _collapse_track_variants(std::string_view local_catalog_id)
    {
        std::map<_track_identity, std::size_t> _selected_indices;
        std::vector<track> _selected_tracks;
        _selected_tracks.reserve(_view._tracks.size());

        for (const track& _candidate : _view._tracks) {
            const _track_identity _key = _identity(_candidate);
            const std::map<_track_identity, std::size_t>::const_iterator _existing = _selected_indices.find(_key);
            if (_existing == _selected_indices.end()) {
                _selected_indices.emplace(_key, _selected_tracks.size());
                _selected_tracks.push_back(_candidate);
                continue;
            }

            track& _current = _selected_tracks[_existing->second];
            if (_prefer_variant(_candidate, _current, local_catalog_id)) {
                _current = _candidate;
            }
        }

        _view._tracks = std::move(_selected_tracks);
    }

    std::string_view _metadata_text(std::string_view value)
    {
        return value.empty() ? std::string_view("Unknown") : value;
    }

    std::string _display_artist(std::string_view value)
    {
        std::string _result;
        std::string_view::size_type _start = 0;
        while (_start <= value.size()) {
            const std::string_view::size_type _separator = value.find('/', _start);
            std::string_view _artist = value.substr(_start, _separator - _start);
            while (!_artist.empty() && std::isspace(static_cast<unsigned char>(_artist.front()))) {
                _artist.remove_prefix(1);
            }
            while (!_artist.empty() && std::isspace(static_cast<unsigned char>(_artist.back()))) {
                _artist.remove_suffix(1);
            }
            if (!_artist.empty()) {
                if (!_result.empty()) {
                    _result += ", ";
                }
                _result.append(_artist.data(), _artist.size());
            }
            if (_separator == std::string_view::npos) {
                break;
            }
            _start = _separator + 1;
        }
        return _result;
    }

    std::string_view _primary_artist(std::string_view value)
    {
        std::string_view _artist = value.substr(0, value.find('/'));
        while (!_artist.empty() && std::isspace(static_cast<unsigned char>(_artist.front()))) {
            _artist.remove_prefix(1);
        }
        while (!_artist.empty() && std::isspace(static_cast<unsigned char>(_artist.back()))) {
            _artist.remove_suffix(1);
        }
        return _metadata_text(_artist);
    }

    std::string_view _search_text(const track& value)
    {
        switch (_view._field) {
        case _search_field::title:
            return value.title;
        case _search_field::artist:
            return value.artist;
        case _search_field::album:
            return value.album;
        }
        return { };
    }

    bool _contains_case_insensitive(std::string_view text, std::string_view search)
    {
        if (search.empty()) {
            return true;
        }
        return std::search(text.begin(), text.end(), search.begin(), search.end(), [](unsigned char left, unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        }) != text.end();
    }

    bool _track_matches_filters(const track& value, const std::unordered_set<std::string>& available_hashes)
    {
        if (_view._offline_only && available_hashes.find(value.file_hash) == available_hashes.end()) {
            return false;
        }
        return _contains_case_insensitive(_search_text(value), _view._search);
    }

    std::unordered_set<std::string> _available_hashes(context& ctx, std::string_view local_catalog_id)
    {
        std::unordered_set<std::string> _hashes;
        if (!_view._offline_only) {
            return _hashes;
        }

        for (const track& _track : _view._tracks) {
            if (_track.catalog_id == local_catalog_id) {
                _hashes.insert(_track.file_hash);
            }
        }
        for (const file_location& _file : ctx.store.managed_files()) {
            _hashes.insert(_file.hash);
        }
        return _hashes;
    }

    void _update_scan_message(context& ctx)
    {
        const library_scan_status _scan = ctx.scanner.status();
        if (_view._scan_state_initialized && _view._scan_state == _scan.state) {
            return;
        }
        _view._scan_state_initialized = true;
        _view._scan_state = _scan.state;

        if (_scan.state == library_scan_state::scanning) {
            ctx.notify("Scanning library...");
        } else if (_scan.state == library_scan_state::completed) {
            char _message[160] { };
            std::snprintf(
                _message,
                sizeof(_message),
                "Library scan complete: %zu files, %zu added, %zu removed, %zu failed.",
                _scan.result.files_found,
                _scan.result.tracks_added,
                _scan.result.tracks_removed,
                _scan.result.files_failed);
            ctx.notify(_message);
        } else if (_scan.state == library_scan_state::failed) {
            ctx.notify(_scan.error_message.empty() ? "Library scan failed." : _scan.error_message, widget_message_kind::error);
        }
    }

    void _update_offline_message(context& ctx, const track& value, const offline_status& status)
    {
        const std::unordered_map<std::string, offline_state>::iterator _previous = _view._offline_states.find(value.file_hash);
        if (_previous != _view._offline_states.end() && _previous->second == status.state) {
            return;
        }

        const bool _was_downloading = _previous != _view._offline_states.end() && _previous->second == offline_state::downloading;
        _view._offline_states.insert_or_assign(value.file_hash, status.state);
        const std::string_view _title = _metadata_text(value.title);
        if (status.state == offline_state::failed) {
            std::string _message = "Download failed for " + std::string(_title);
            if (!status.error_message.empty()) {
                _message += ": " + status.error_message;
            }
            ctx.notify(std::move(_message), widget_message_kind::error);
        } else if (status.state == offline_state::on && _was_downloading) {
            ctx.notify("Available offline: " + std::string(_title) + ".");
        }
    }

    int _compare_text(std::string_view left, std::string_view right)
    {
        const std::size_t _common_size = (std::min)(left.size(), right.size());
        for (std::size_t _index = 0; _index < _common_size; ++_index) {
            const int _left_value = std::tolower(static_cast<unsigned char>(left[_index]));
            const int _right_value = std::tolower(static_cast<unsigned char>(right[_index]));
            if (_left_value != _right_value) {
                return _left_value < _right_value ? -1 : 1;
            }
        }
        return left.size() < right.size() ? -1 : right.size() < left.size() ? 1
                                                                            : 0;
    }

    void _sort_tracks(std::vector<track>& tracks)
    {
        std::sort(tracks.begin(), tracks.end(), [](const track& left, const track& right) {
            const int _artist = _compare_text(_primary_artist(left.artist), _primary_artist(right.artist));
            if (_artist != 0) {
                return _artist < 0;
            }

            const int _album = _compare_text(_metadata_text(left.album), _metadata_text(right.album));
            if (_album != 0) {
                return _album < 0;
            }

            if (left.track_number != right.track_number) {
                if (left.track_number == 0) {
                    return false;
                }
                if (right.track_number == 0) {
                    return true;
                }
                return left.track_number < right.track_number;
            }

            const int _title = _compare_text(_metadata_text(left.title), _metadata_text(right.title));
            if (_title != 0) {
                return _title < 0;
            }
            return std::tie(left.catalog_id, left.id) < std::tie(right.catalog_id, right.id);
        });
    }

    std::string _duration_text(std::uint64_t milliseconds)
    {
        if (milliseconds == 0) {
            return "Unknown";
        }
        const std::uint64_t _seconds = milliseconds / 1'000;
        const std::uint64_t _minutes = _seconds / 60;
        char _value[32] { };
        std::snprintf(_value, sizeof(_value), "%llu:%02llu", static_cast<unsigned long long>(_minutes), static_cast<unsigned long long>(_seconds % 60));
        return _value;
    }

    const peer_record* _find_peer(const std::vector<peer_record>& peers, std::string_view id)
    {
        const std::vector<peer_record>::const_iterator _match = std::find_if(peers.begin(), peers.end(), [id](const peer_record& value) {
            return value.id == id;
        });
        return _match == peers.end() ? nullptr : &*_match;
    }

    std::shared_ptr<audio_source> _track_source(context& ctx, const track& value, const std::vector<peer_record>& peers)
    {
        const std::optional<file_location> _file = ctx.store.find_file(value.file_hash);
        if (_file) {
            return std::make_shared<file_audio_source>(std::filesystem::u8path(_file->path));
        }

        const peer_record* _owner = _find_peer(peers, value.catalog_id);
        if (_owner == nullptr) {
            throw widget_error("The selected track is not available on this device or a paired instance");
        }
        if (_owner->token.empty()) {
            throw widget_error("Pair with this instance again before streaming its tracks");
        }
        if (_owner->fingerprint.empty()) {
            throw widget_error("Pair with this instance again before connecting securely");
        }
        if (_owner->endpoints.empty()) {
            throw widget_error("This friend has no known network endpoint");
        }

        std::shared_ptr<peer_client> _client = std::make_shared<peer_client>(ctx.store, *_owner);
        return std::make_shared<stream_audio_source>(std::move(_client), value.file_hash, value.size_bytes);
    }

    void _select_track(context& ctx, const track& value, const std::vector<peer_record>& peers, bool start_playback)
    {
        _view._selected_catalog_id = value.catalog_id;
        _view._selected_track_id = value.id;
        ctx.current_track = value;
        ctx.try_action([&ctx, &value, &peers, start_playback] {
            ctx.store.set_playback_selection(value);
            std::shared_ptr<audio_source> _source = _track_source(ctx, value, peers);
            ctx.player.open(std::move(_source), value.extension);
            if (start_playback) {
                ctx.player.play();
            }
        });
    }

    void _play_track(context& ctx, const track& value, const std::vector<peer_record>& peers)
    {
        _select_track(ctx, value, peers, true);
    }

    const track* _adjacent_track(const context& ctx, int direction)
    {
        if (!ctx.current_track || _view._visible_track_indices.empty()) {
            return nullptr;
        }
        const std::vector<std::size_t>::const_iterator _current = std::find_if(_view._visible_track_indices.begin(), _view._visible_track_indices.end(), [&ctx](std::size_t index) {
            const track& _candidate = _view._tracks[index];
            return _candidate.catalog_id == ctx.current_track->catalog_id && _candidate.id == ctx.current_track->id;
        });
        if (_current == _view._visible_track_indices.end()) {
            return nullptr;
        }
        if (direction < 0) {
            if (_current == _view._visible_track_indices.begin()) {
                return nullptr;
            }
            return &_view._tracks[*std::prev(_current)];
        }
        const std::vector<std::size_t>::const_iterator _next = std::next(_current);
        return _next == _view._visible_track_indices.end() ? nullptr : &_view._tracks[*_next];
    }

    void _play_adjacent_track(context& ctx, int direction)
    {
        const track* _target = _adjacent_track(ctx, direction);
        if (_target != nullptr) {
            const std::vector<peer_record> _peers = ctx.store.peers();
            _play_track(ctx, *_target, _peers);
        }
    }

    void _restore_selection(context& ctx, const std::vector<peer_record>& peers)
    {
        if (_view._tracks.empty()) {
            _view._selected_catalog_id.clear();
            _view._selected_track_id.clear();
            ctx.current_track.reset();
            return;
        }

        std::optional<track> _preferred = ctx.current_track;
        if (!_preferred) {
            _preferred = ctx.store.playback_selection();
        }

        std::vector<track>::const_iterator _selected = _view._tracks.end();
        if (_preferred) {
            _selected = std::find_if(_view._tracks.begin(), _view._tracks.end(), [&_preferred](const track& value) {
                return value.catalog_id == _preferred->catalog_id && value.id == _preferred->id;
            });
            if (_selected == _view._tracks.end()) {
                _selected = std::find_if(_view._tracks.begin(), _view._tracks.end(), [&_preferred](const track& value) {
                    return _same_recording(value, *_preferred);
                });
            }
        }

        if (_selected == _view._tracks.end()) {
            _select_track(ctx, _view._tracks.front(), peers, false);
            return;
        }

        _view._selected_catalog_id = _selected->catalog_id;
        _view._selected_track_id = _selected->id;
        if (ctx.current_track && ctx.current_track->catalog_id == _selected->catalog_id && ctx.current_track->id == _selected->id) {
            ctx.current_track = *_selected;
        } else {
            _select_track(ctx, *_selected, peers, false);
        }
    }

    void _draw_track_menu(context& ctx, track& value, const offline_status& status)
    {
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupContextItem("TrackActions")) {
            return;
        }

        if (ImGui::IsWindowAppearing()) {
            _editor._title = value.title;
            _editor._artist = value.artist;
            _editor._album = value.album;
            _editor._track_number = value.track_number;
        }

        const bool _local_file = status.state == offline_state::on && !status.can_remove;
        if (_local_file) {
            ImGui::TextDisabled("Local");
        } else {
            bool _saved = status.requested;
            if (animation_toggle("Save offline", &_saved)) {
                const bool _updated = ctx.try_action([&ctx, &value, _saved] {
                    if (_saved) {
                        ctx.offline.save(value);
                    } else {
                        ctx.offline.remove(value);
                    }
                });
                if (_updated) {
                    const std::string _title(_metadata_text(value.title));
                    ctx.notify(_saved ? "Downloading " + _title + "..." : "Offline copy removed for " + _title + ".");
                }
            }
        }

        bool _metadata_changed = false;
        ImGui::SetNextItemWidth(230.0f);
        ImGui::InputText("Title", &_editor._title);
        _metadata_changed = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(230.0f);
        ImGui::InputText("Artist", &_editor._artist);
        _metadata_changed = ImGui::IsItemDeactivatedAfterEdit() || _metadata_changed;
        ImGui::SetNextItemWidth(230.0f);
        ImGui::InputText("Album", &_editor._album);
        _metadata_changed = ImGui::IsItemDeactivatedAfterEdit() || _metadata_changed;
        ImGui::SetNextItemWidth(230.0f);
        ImGui::InputScalar("Track number", ImGuiDataType_U32, &_editor._track_number);
        _metadata_changed = ImGui::IsItemDeactivatedAfterEdit() || _metadata_changed;
        if (_metadata_changed) {
            const bool _updated = ctx.try_action([&ctx, &value] {
                ctx.store.update_track_metadata(value.catalog_id, value.id, _editor._title, _editor._artist, _editor._album, _editor._track_number);
            });
            if (_updated) {
                value.title = _editor._title;
                value.artist = _editor._artist;
                value.album = _editor._album;
                value.track_number = _editor._track_number;
                _view._tracks_dirty = true;
                if (ctx.current_track && ctx.current_track->catalog_id == value.catalog_id && ctx.current_track->id == value.id) {
                    ctx.current_track = value;
                }
                ctx.notify("Metadata updated.");
            }
        }

        if (status.state == offline_state::failed) {
            const float _button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button(icons::retry_download, ImVec2(_button_width, 0.0f))) {
                if (ctx.try_action([&ctx, &value] { ctx.offline.save(value); })) {
                    ctx.notify("Retrying download for " + std::string(_metadata_text(value.title)) + "...");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(icons::cancel_download, ImVec2(_button_width, 0.0f))) {
                if (ctx.try_action([&ctx, &value] { ctx.offline.remove(value); })) {
                    ctx.notify("Download cancelled for " + std::string(_metadata_text(value.title)) + ".");
                }
            }
        }

        ImGui::EndPopup();
    }

    std::string _offline_status_text(const offline_status& status, float animated_progress)
    {
        switch (status.state) {
        case offline_state::off:
            return { };
        case offline_state::on:
            return status.can_remove ? "Offline" : "Local";
        case offline_state::downloading:
            if (status.total_bytes != 0) {
                const unsigned _percentage = static_cast<unsigned>(
                    (std::clamp)(animated_progress, 0.0f, 1.0f) * 100.0f + 0.5f);
                return "Downloading " + std::to_string(_percentage) + "%";
            }
            return "Downloading";
        case offline_state::failed:
            return "Download failed";
        }
        return { };
    }

    void _draw_track_card(context& ctx, track& value, const std::vector<peer_record>& peers)
    {
        const offline_status _offline = ctx.offline.status(value);
        _update_offline_message(ctx, value, _offline);
        ImGui::PushID(value.catalog_id.c_str());
        ImGui::PushID(value.id.c_str());
        const ImGuiID _motion_owner = ImGui::GetID("##TrackCardMotion");
        constexpr float _card_height = 72.0f;
        constexpr float _cover_size = 56.0f;
        const ImVec2 _position = ImGui::GetCursorScreenPos();
        const float _width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x + _card_right_extension);
        const ImVec2 _end(_position.x + _width, _position.y + _card_height);
        const std::string _duration = _duration_text(value.duration_ms);
        const ImVec2 _duration_size = ImGui::CalcTextSize(_duration.c_str());
        const float _duration_x = _end.x - _track_card_padding - _duration_size.x;
        const float _download_target = _offline.state == offline_state::downloading && _offline.total_bytes != 0 ? static_cast<float>((std::min)(static_cast<double>(_offline.downloaded_bytes) / static_cast<double>(_offline.total_bytes), 1.0)) : 0.0f;
        const float _download_progress = animation_tween(_motion_owner, 0x12001u, _download_target, animation_relaxed, iam_ease_out_cubic);
        const std::string _offline_text = _offline_status_text(_offline, _download_progress);
        const ImVec2 _offline_size = ImGui::CalcTextSize(_offline_text.c_str());
        const bool _downloading = _offline.state == offline_state::downloading;
        const float _offline_indicator_width = _downloading ? 16.0f : 0.0f;
        const float _offline_x = _duration_x - ImGui::GetStyle().ItemSpacing.x - _offline_size.x - _offline_indicator_width;
        const float _right_x = _offline_text.empty() ? _duration_x : _offline_x;
        const bool _selected = _view._selected_catalog_id == value.catalog_id && _view._selected_track_id == value.id;

        const bool _card_pressed = ImGui::InvisibleButton("##TrackCard", ImVec2(_width, _card_height));
#ifdef __ANDROID__
        if (_card_pressed && !_view._touch_scroll_moved) {
#else
        if (_card_pressed) {
#endif
            _play_track(ctx, value, peers);
        }
#ifdef __ANDROID__
        constexpr bool _hovered = false;
#else
        const bool _hovered = ImGui::IsItemHovered();
#endif
        _draw_track_menu(ctx, value, _offline);

        const float _hover = animation_tween(_motion_owner, 0x12002u, _hovered ? 1.0f : 0.0f, animation_quick, iam_ease_out_cubic);
        const float _selection = animation_tween(_motion_owner, 0x12003u, _selected ? 1.0f : 0.0f, animation_normal, iam_ease_out_cubic);

        ImDrawList* _draw = ImGui::GetWindowDrawList();
        const float _rounding = ImGui::GetStyle().FrameRounding;
        const float _background_alpha = (std::max)(_hover, _selection);
        if (_background_alpha > 0.001f) {
            const ImVec4 _background = iam_get_blended_color(ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered), ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive), _selection, iam_col_oklab);
            _draw->AddRectFilled(_position, _end, animation_with_alpha(_background, _background_alpha), _rounding);
        }
        if (_selection > 0.001f) {
            _draw->AddRect(_position, _end, animation_with_alpha(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), _selection), _rounding, 0, 1.0f + _selection);
        }

        const float _cover_lift = -1.5f * _hover;
        const ImVec2 _cover_min(_position.x + _track_card_padding, _position.y + _track_card_padding + _cover_lift);
        const ImVec2 _cover_max(_cover_min.x + _cover_size, _cover_min.y + _cover_size);
        const std::optional<renderer_texture> _cover = ctx.covers.texture(value);
        if (_cover) {
            _draw->AddImageRounded(reinterpret_cast<ImTextureID>(_cover->native_id), _cover_min, _cover_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE, _rounding);
        } else {
            _draw->AddRectFilled(_cover_min, _cover_max, IM_COL32_WHITE, _rounding);
        }
        if (_hover > 0.001f) {
            _draw->AddRect(_cover_min, _cover_max, animation_with_alpha(ImGui::GetStyleColorVec4(ImGuiCol_Text), _hover * 0.28f), _rounding, 0, 1.0f);
        }

        ImFont* _title_font = ctx.fonts.track_title != nullptr ? ctx.fonts.track_title : ImGui::GetFont();
        ImFont* _subtitle_font = ctx.fonts.subtitle != nullptr ? ctx.fonts.subtitle : ImGui::GetFont();
        const ImU32 _text_color = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 _secondary_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const float _text_x = _cover_max.x + _cover_text_spacing;
        const ImVec2 _clip_min(_text_x, _position.y);
        const ImVec2 _clip_max((std::max)(_text_x + 1.0f, _right_x - _track_card_padding), _end.y);

        const std::string_view _title = _metadata_text(value.title);
        std::string _artist_album = _display_artist(_metadata_text(value.artist)) + " - " + std::string(_metadata_text(value.album));
        if (value.track_number != 0) {
            _artist_album += "  #" + std::to_string(value.track_number);
        }
        _draw->PushClipRect(_clip_min, _clip_max, true);
        _draw->AddText(_title_font, _title_font->FontSize, ImVec2(_text_x, _position.y + 15.0f), _text_color, _title.data(), _title.data() + _title.size());
        _draw->AddText(_subtitle_font, _subtitle_font->FontSize, ImVec2(_text_x, _position.y + 39.0f), _secondary_color, _artist_album.c_str());
        _draw->PopClipRect();

        _draw->AddText(ImVec2(_duration_x, _position.y + (_card_height - _duration_size.y) * 0.5f), _secondary_color, _duration.c_str());
        if (!_offline_text.empty()) {
            ImGui::PushID(static_cast<int>(_offline.state));
            const ImGuiID _status_owner = ImGui::GetID("##OfflineStatusMotion");
            ImGui::PopID();
            const float _status_alpha = animation_tween(_status_owner, 0x12004u, 1.0f, animation_normal, iam_ease_out_cubic);
            const ImVec4 _offline_color = _offline.state == offline_state::failed ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            const float _status_y = _position.y + (_card_height - _offline_size.y) * 0.5f;
            if (_downloading) {
                animation_activity_indicator(_status_owner, ImVec2(_offline_x + 6.0f, _status_y + _offline_size.y * 0.5f), 5.0f, animation_with_alpha(_offline_color, _status_alpha));
            }
            _draw->AddText(ImVec2(_offline_x + _offline_indicator_width, _status_y + (1.0f - _status_alpha) * 3.0f), animation_with_alpha(_offline_color, _status_alpha), _offline_text.c_str());
        }
        ImGui::PopID();
        ImGui::PopID();
    }

    ImVec4 _chrome_background()
    {
        return ImVec4(0.025f, 0.025f, 0.025f, 0.82f);
    }

    void _draw_library_edge_fades(ImVec2 minimum, ImVec2 maximum)
    {
        constexpr float _fade_height = 28.0f;
        const ImVec4 _background = _chrome_background();
        const ImU32 _edge = ImGui::ColorConvertFloat4ToU32(_background);
        const ImU32 _transparent = ImGui::ColorConvertFloat4ToU32(ImVec4(_background.x, _background.y, _background.z, 0.0f));
        ImDrawList* _draw = ImGui::GetWindowDrawList();

        const float _top_end_y = (std::min)(maximum.y, minimum.y + _fade_height);
        _draw->AddRectFilledMultiColor(minimum, ImVec2(maximum.x, _top_end_y), _edge, _edge, _transparent, _transparent);

        const float _bottom_start_y = (std::max)(minimum.y, maximum.y - _fade_height);
        _draw->AddRectFilledMultiColor(ImVec2(minimum.x, _bottom_start_y), maximum, _transparent, _transparent, _edge, _edge);
    }

#ifdef __ANDROID__
    void _handle_touch_scroll(ImVec2 minimum, ImVec2 maximum)
    {
        ImGuiIO& _io = ImGui::GetIO();
        if (_io.MouseSource != ImGuiMouseSource_TouchScreen) {
            _view._touch_scroll_active = false;
            return;
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && ImGui::IsMouseHoveringRect(minimum, maximum, false)) {
            _view._touch_scroll_active = true;
            _view._touch_scroll_moved = false;
            _view._touch_scroll_start_y = _io.MousePos.y;
            _view._touch_scroll_previous_y = _io.MousePos.y;
        }

        if (!_view._touch_scroll_active) {
            return;
        }

        constexpr float _drag_threshold = 8.0f;
        if (!_view._touch_scroll_moved
            && std::abs(_io.MousePos.y - _view._touch_scroll_start_y) >= _drag_threshold) {
            _view._touch_scroll_moved = true;
        }

        if (_view._touch_scroll_moved) {
            const float _delta_y = _io.MousePos.y - _view._touch_scroll_previous_y;
            const float _scroll_y = (std::clamp)(
                ImGui::GetScrollY() - _delta_y,
                0.0f,
                ImGui::GetScrollMaxY());
            ImGui::SetScrollY(_scroll_y);
        }
        _view._touch_scroll_previous_y = _io.MousePos.y;

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            _view._touch_scroll_active = false;
        }
    }
#endif

    void _draw_track_cards(context& ctx, const std::vector<peer_record>& peers, const std::unordered_set<std::string>& available_hashes)
    {
        if (_view._tracks_dirty) {
            _sort_tracks(_view._tracks);
            _view._tracks_dirty = false;
        }
        if (_view._selection_dirty) {
            _restore_selection(ctx, peers);
            _view._selection_dirty = false;
        }

        _view._visible_track_indices.clear();
        _view._visible_track_indices.reserve(_view._tracks.size());
        for (std::size_t _index = 0; _index < _view._tracks.size(); ++_index) {
            if (_track_matches_filters(_view._tracks[_index], available_hashes)) {
                _view._visible_track_indices.push_back(_index);
            }
        }

        const ImVec2 _list_min = ImGui::GetCursorScreenPos();
        const ImVec2 _list_size = ImGui::GetContentRegionAvail();
        const ImVec2 _list_max(_list_min.x + _list_size.x, _list_min.y + _list_size.y);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(_library_list_horizontal_padding, 0.0f));
        const bool _list_visible = ImGui::BeginChild("##LibraryCards", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        if (!_list_visible) {
            ImGui::EndChild();
            return;
        }
#ifdef __ANDROID__
        _handle_touch_scroll(_list_min, _list_max);
#endif
        if (_view._visible_track_indices.empty()) {
            ImGui::TextDisabled(_view._tracks.empty() ? "No tracks in this library" : "No tracks match the current filters");
        }

        ImGuiListClipper _clipper;
        _clipper.Begin(static_cast<int>(_view._visible_track_indices.size()));
        while (_clipper.Step()) {
            for (int _row = _clipper.DisplayStart; _row < _clipper.DisplayEnd; ++_row) {
                const std::size_t _track_index = _view._visible_track_indices[static_cast<std::size_t>(_row)];
                _draw_track_card(ctx, _view._tracks[_track_index], peers);
            }
        }
        _draw_library_edge_fades(_list_min, _list_max);
        ImGui::EndChild();
    }

    void _append_catalog(context& ctx, std::string_view id, std::string name)
    {
        const std::optional<catalog_snapshot> _catalog = ctx.store.catalog(id);
        if (!_catalog) {
            return;
        }
        _view._catalog_names.emplace(std::string(id), std::move(name));
        _view._tracks.insert(_view._tracks.end(), _catalog->tracks.begin(), _catalog->tracks.end());
    }

    void _refresh_tracks(context& ctx, const instance_info& instance, const std::vector<peer_record>& peers)
    {
        std::string _revision_key;
        const std::optional<std::uint64_t> _local_revision = ctx.store.catalog_revision(instance.id);
        _revision_key = instance.id + ":" + instance.name + ":";
        _revision_key += _local_revision ? std::to_string(*_local_revision) : "missing";

        for (const peer_record& _record : peers) {
            if (!_record.library_enabled) {
                continue;
            }
            const std::optional<std::uint64_t> _revision = ctx.store.catalog_revision(_record.id);
            _revision_key += ";" + _record.id + ":" + _record.name + ":";
            _revision_key += _revision ? std::to_string(*_revision) : "missing";
        }

        if (_view._revision_key == _revision_key) {
            return;
        }

        _view._revision_key = std::move(_revision_key);
        _view._catalog_names.clear();
        _view._tracks.clear();
        _append_catalog(ctx, instance.id, instance.name + " (This device)");
        for (const peer_record& _record : peers) {
            if (_record.library_enabled) {
                _append_catalog(ctx, _record.id, _record.name);
            }
        }
        _collapse_track_variants(instance.id);
        _view._tracks_dirty = true;
        _view._selection_dirty = true;
    }

}

bool can_play_previous_track(const context& ctx)
{
    return _adjacent_track(ctx, -1) != nullptr;
}

bool can_play_next_track(const context& ctx)
{
    return _adjacent_track(ctx, 1) != nullptr;
}

void play_previous_track(context& ctx)
{
    _play_adjacent_track(ctx, -1);
}

void play_next_track(context& ctx)
{
    _play_adjacent_track(ctx, 1);
}

void draw_library(context& ctx)
{
    const instance_info _instance = ctx.store.instance();
    const std::vector<peer_record> _peers = ctx.store.peers();
    _refresh_tracks(ctx, _instance, _peers);
    _update_scan_message(ctx);

    ImDrawList* _draw = ImGui::GetWindowDrawList();
    const ImVec2 _chrome_min = ImGui::GetWindowPos();
    const float _chrome_right = _chrome_min.x + ImGui::GetWindowSize().x;
    _draw->ChannelsSplit(2);
    _draw->ChannelsSetCurrent(1);

    const float _toolbar_inset = _library_list_horizontal_padding + _track_card_padding;
    const ImVec2 _toolbar_position = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(_toolbar_position.x + _toolbar_inset, _toolbar_position.y + _toolbar_inset + ImGui::GetStyle().WindowPadding.y));
    const float _toolbar_start_x = ImGui::GetCursorPosX();
    const float _toolbar_width = ImGui::GetContentRegionAvail().x;
    const float _toolbar_right_x = _toolbar_start_x + _toolbar_width - _library_list_horizontal_padding - ImGui::GetStyle().ScrollbarSize + _card_right_extension;

    const std::unordered_set<std::string> _available = _available_hashes(ctx, _instance.id);
    const std::size_t _visible_track_count = static_cast<std::size_t>(std::count_if(_view._tracks.begin(), _view._tracks.end(), [&_available](const track& value) {
        return _track_matches_filters(value, _available);
    }));
    const std::string _info_text = std::to_string(_visible_track_count) + " tracks from " + std::to_string(_view._catalog_names.size()) + " libraries";

    const ImGuiStyle& _style = ImGui::GetStyle();
    constexpr float _search_width = 240.0f;
    constexpr float _search_field_width = 90.0f;
    const float _offline_width = ImGui::CalcTextSize(icons::offline_only).x + _style.FramePadding.x * 2.0f;
    const float _info_width = ImGui::CalcTextSize(_info_text.c_str()).x;
    const float _friends_width = ImGui::CalcTextSize(icons::friends).x + _style.FramePadding.x * 2.0f;
    const float _settings_width = ImGui::CalcTextSize(icons::settings).x + _style.FramePadding.x * 2.0f;
    const float _buttons_width = _friends_width + _style.ItemSpacing.x + _settings_width;
    const float _optional_width = (std::max)(0.0f, _toolbar_right_x - _toolbar_start_x - _buttons_width - _style.ItemSpacing.x);

    float _priority_width = _search_width;
    const bool _show_search = _optional_width >= _priority_width;
    _priority_width += _style.ItemSpacing.x + _search_field_width;
    const bool _show_search_field = _show_search && _optional_width >= _priority_width;
    _priority_width += _style.ItemSpacing.x + _offline_width;
    const bool _show_offline = _show_search_field && _optional_width >= _priority_width;
    _priority_width += _style.ItemSpacing.x + _info_width;
    const bool _show_info = _show_offline && _optional_width >= _priority_width;

    bool _has_optional_item = false;
    if (_show_offline) {
        _draw_offline_toggle(_view._offline_only);
        _has_optional_item = true;
    }
    if (_show_search_field) {
        if (_has_optional_item) {
            ImGui::SameLine();
        }
        constexpr const char* _search_fields[] = { "Title", "Artist", "Album" };
        int _selected_field = static_cast<int>(_view._field);
        ImGui::SetNextItemWidth(_search_field_width);
        if (ImGui::Combo("##SearchField", &_selected_field, _search_fields, 3)) {
            _view._field = static_cast<_search_field>(_selected_field);
        }
        _has_optional_item = true;
    }
    if (_show_search) {
        if (_has_optional_item) {
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(_search_width);
        ImGui::InputTextWithHint("##TrackSearch", icons::search_hint, &_view._search);
        _has_optional_item = true;
    }
    if (_show_info) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", _info_text.c_str());
    }

    if (_has_optional_item) {
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(_toolbar_right_x - _buttons_width);
    if (ImGui::Button(icons::friends, ImVec2(_friends_width, 0.0f))) {
        open_friends(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button(icons::settings, ImVec2(_settings_width, 0.0f))) {
        open_settings(ctx);
    }

    ImGui::SetCursorPosX(_toolbar_start_x);
    const widget_message* _message = ctx.active_message();
    if (_message != nullptr) {
        _message_visual._text = _message->text;
        _message_visual._kind = _message->kind;
    }
    ImGui::PushID(_message_visual._text.c_str());
    const ImGuiID _message_owner = ImGui::GetID("##MessageMotion");
    const float _message_alpha = animation_tween(_message_owner, 0x13001u, _message != nullptr ? 1.0f : 0.0f, animation_normal, iam_ease_out_cubic);
    ImGui::PopID();
    const ImVec2 _message_position = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    if (!_message_visual._text.empty() && _message_alpha > 0.001f) {
        const ImVec4 _message_color = _message_visual._kind == widget_message_kind::error ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        const bool _show_activity = _message_visual._kind == widget_message_kind::progress
            && (_message_visual._text.rfind("Scanning", 0) == 0
                || _message_visual._text.rfind("Refreshing", 0) == 0
                || _message_visual._text.rfind("Downloading", 0) == 0
                || _message_visual._text.rfind("Retrying", 0) == 0);
        const float _activity_width = _show_activity ? 16.0f : 0.0f;
        if (_show_activity) {
            animation_activity_indicator(_message_owner, ImVec2(_message_position.x + 5.0f, _message_position.y + ImGui::GetTextLineHeight() * 0.5f), 4.5f, animation_with_alpha(_message_color, _message_alpha));
        }
        _draw->AddText(ImVec2(_message_position.x + _activity_width, _message_position.y + (1.0f - _message_alpha) * 5.0f), animation_with_alpha(_message_color, _message_alpha), _message_visual._text.c_str());
    }

    const float _cards_top = ImGui::GetCursorScreenPos().y;
    _draw->ChannelsSetCurrent(0);
    _draw->AddRectFilled(_chrome_min, ImVec2(_chrome_right, _cards_top), ImGui::GetColorU32(_chrome_background()));
    _draw->ChannelsMerge();

    _draw_track_cards(ctx, _peers, _available);
}

}
