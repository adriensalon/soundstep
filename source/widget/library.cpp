#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <core/context.hpp>
#include <widget/friends.hpp>
#include <widget/library.hpp>
#include <widget/settings.hpp>

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
    };

    _library_view_state _view;

    struct _track_editor_state {
        std::string _title;
        std::string _artist;
        std::string _album;
        std::uint32_t _track_number { 0 };
    };

    _track_editor_state _editor;

    constexpr float _library_list_horizontal_padding = 12.0f;
    constexpr float _track_card_padding = 8.0f;

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
        return duration_ms % _bucket_size_ms >= _bucket_size_ms / 2
            ? _bucket + 1
            : _bucket;
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

    bool _prefer_variant(
        const track& candidate,
        const track& current,
        std::string_view local_catalog_id)
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
        return std::tie(candidate.catalog_id, candidate.id)
            < std::tie(current.catalog_id, current.id);
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
        return std::search(
                   text.begin(),
                   text.end(),
                   search.begin(),
                   search.end(),
                   [](unsigned char left, unsigned char right) {
                       return std::tolower(left) == std::tolower(right);
                   })
            != text.end();
    }

    bool _track_matches_filters(
        const track& value,
        const std::unordered_set<std::string>& available_hashes)
    {
        if (_view._offline_only
            && available_hashes.find(value.file_hash) == available_hashes.end()) {
            return false;
        }
        return _contains_case_insensitive(_search_text(value), _view._search);
    }

    std::unordered_set<std::string> _available_hashes(
        context& ctx,
        std::string_view local_catalog_id)
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
            ctx.notify(
                _scan.error_message.empty() ? "Library scan failed." : _scan.error_message,
                widget_message_kind::error);
        }
    }

    void _update_offline_message(
        context& ctx,
        const track& value,
        const offline_status& status)
    {
        const std::unordered_map<std::string, offline_state>::iterator _previous = _view._offline_states.find(value.file_hash);
        if (_previous != _view._offline_states.end()
            && _previous->second == status.state) {
            return;
        }

        const bool _was_downloading = _previous != _view._offline_states.end()
            && _previous->second == offline_state::downloading;
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
            const int _artist = _compare_text(
                _metadata_text(left.artist),
                _metadata_text(right.artist));
            if (_artist != 0) {
                return _artist < 0;
            }

            const int _album = _compare_text(
                _metadata_text(left.album),
                _metadata_text(right.album));
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

            const int _title = _compare_text(
                _metadata_text(left.title),
                _metadata_text(right.title));
            if (_title != 0) {
                return _title < 0;
            }
            return std::tie(left.catalog_id, left.id)
                < std::tie(right.catalog_id, right.id);
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
        std::snprintf(
            _value,
            sizeof(_value),
            "%llu:%02llu",
            static_cast<unsigned long long>(_minutes),
            static_cast<unsigned long long>(_seconds % 60));
        return _value;
    }

    const peer_record* _find_peer(const std::vector<peer_record>& peers, std::string_view id)
    {
        const std::vector<peer_record>::const_iterator _match = std::find_if(
            peers.begin(),
            peers.end(),
            [id](const peer_record& value) { return value.id == id; });
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
            _selected = std::find_if(
                _view._tracks.begin(),
                _view._tracks.end(),
                [&_preferred](const track& value) {
                    return value.catalog_id == _preferred->catalog_id
                        && value.id == _preferred->id;
                });
            if (_selected == _view._tracks.end()) {
                _selected = std::find_if(
                    _view._tracks.begin(),
                    _view._tracks.end(),
                    [&_preferred](const track& value) {
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
        if (ctx.current_track
            && ctx.current_track->catalog_id == _selected->catalog_id
            && ctx.current_track->id == _selected->id) {
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
            if (ImGui::Checkbox("Save offline", &_saved)) {
                const bool _updated = ctx.try_action([&ctx, &value, _saved] {
                    if (_saved) {
                        ctx.offline.save(value);
                    } else {
                        ctx.offline.remove(value);
                    }
                });
                if (_updated) {
                    const std::string _title(_metadata_text(value.title));
                    ctx.notify(
                        _saved
                            ? "Downloading " + _title + "..."
                            : "Offline copy removed for " + _title + ".");
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
                ctx.store.update_track_metadata(
                    value.catalog_id,
                    value.id,
                    _editor._title,
                    _editor._artist,
                    _editor._album,
                    _editor._track_number);
            });
            if (_updated) {
                value.title = _editor._title;
                value.artist = _editor._artist;
                value.album = _editor._album;
                value.track_number = _editor._track_number;
                _view._tracks_dirty = true;
                if (ctx.current_track
                    && ctx.current_track->catalog_id == value.catalog_id
                    && ctx.current_track->id == value.id) {
                    ctx.current_track = value;
                }
                ctx.notify("Metadata updated.");
            }
        }

        if (status.state == offline_state::failed) {
            const float _button_width = (ImGui::GetContentRegionAvail().x
                                            - ImGui::GetStyle().ItemSpacing.x)
                * 0.5f;
            if (ImGui::Button("Retry download", ImVec2(_button_width, 0.0f))) {
                if (ctx.try_action([&ctx, &value] { ctx.offline.save(value); })) {
                    ctx.notify("Retrying download for "
                        + std::string(_metadata_text(value.title)) + "...");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel download", ImVec2(_button_width, 0.0f))) {
                if (ctx.try_action([&ctx, &value] { ctx.offline.remove(value); })) {
                    ctx.notify("Download cancelled for "
                        + std::string(_metadata_text(value.title)) + ".");
                }
            }
        }

        ImGui::EndPopup();
    }

    std::string _offline_status_text(const offline_status& status)
    {
        switch (status.state) {
        case offline_state::off:
            return { };
        case offline_state::on:
            return status.can_remove ? "Offline" : "Local";
        case offline_state::downloading:
            if (status.total_bytes != 0) {
                const unsigned _percentage = static_cast<unsigned>(
                    (std::min)(static_cast<double>(status.downloaded_bytes) * 100.0
                            / static_cast<double>(status.total_bytes),
                        100.0));
                return "Downloading " + std::to_string(_percentage) + "%";
            }
            return "Downloading";
        case offline_state::failed:
            return "Download failed";
        }
        return { };
    }

    void _draw_track_card(
        context& ctx,
        track& value,
        const std::vector<peer_record>& peers)
    {
        const offline_status _offline = ctx.offline.status(value);
        _update_offline_message(ctx, value, _offline);

        ImGui::PushID(value.catalog_id.c_str());
        ImGui::PushID(value.id.c_str());
        constexpr float _card_height = 72.0f;
        constexpr float _cover_size = 56.0f;
        const ImVec2 _position = ImGui::GetCursorScreenPos();
        const float _width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
        const ImVec2 _end(_position.x + _width, _position.y + _card_height);
        const std::string _duration = _duration_text(value.duration_ms);
        const ImVec2 _duration_size = ImGui::CalcTextSize(_duration.c_str());
        const float _duration_x = _end.x - _track_card_padding - _duration_size.x;
        const std::string _offline_text = _offline_status_text(_offline);
        const ImVec2 _offline_size = ImGui::CalcTextSize(_offline_text.c_str());
        const float _offline_x = _duration_x - ImGui::GetStyle().ItemSpacing.x
            - _offline_size.x;
        const float _right_x = _offline_text.empty() ? _duration_x : _offline_x;
        const bool _selected = _view._selected_catalog_id == value.catalog_id
            && _view._selected_track_id == value.id;

        if (ImGui::InvisibleButton("##TrackCard", ImVec2(_width, _card_height))) {
            _play_track(ctx, value, peers);
        }
        const bool _hovered = ImGui::IsItemHovered();
        _draw_track_menu(ctx, value, _offline);

        ImDrawList* _draw = ImGui::GetWindowDrawList();
        if (_selected || _hovered) {
            const ImU32 _background = ImGui::GetColorU32(
                _selected ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered);
            _draw->AddRectFilled(_position, _end, _background, 6.0f);
        }
        if (_selected) {
            _draw->AddRect(
                _position,
                _end,
                ImGui::GetColorU32(ImGuiCol_CheckMark),
                6.0f,
                0,
                2.0f);
        }

        const ImVec2 _cover_min(
            _position.x + _track_card_padding,
            _position.y + _track_card_padding);
        const ImVec2 _cover_max(_cover_min.x + _cover_size, _cover_min.y + _cover_size);
        const std::optional<renderer_texture> _cover = ctx.covers.texture(value);
        if (_cover) {
            _draw->AddImage(
                reinterpret_cast<ImTextureID>(_cover->native_id),
                _cover_min,
                _cover_max);
        } else {
            _draw->AddRectFilled(_cover_min, _cover_max, IM_COL32_WHITE, 4.0f);
        }

        ImFont* _body_font = ctx.fonts.body != nullptr ? ctx.fonts.body : ImGui::GetFont();
        ImFont* _emphasis_font = ctx.fonts.emphasis != nullptr ? ctx.fonts.emphasis : _body_font;
        const ImU32 _text_color = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 _secondary_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const float _text_x = _cover_max.x + _track_card_padding;
        const ImVec2 _clip_min(_text_x, _position.y);
        const ImVec2 _clip_max(
            (std::max)(_text_x + 1.0f, _right_x - _track_card_padding),
            _end.y);

        const std::string_view _title = _metadata_text(value.title);
        std::string _artist_album = std::string(_metadata_text(value.artist))
            + " - " + std::string(_metadata_text(value.album));
        if (value.track_number != 0) {
            _artist_album += "  #" + std::to_string(value.track_number);
        }
        _draw->PushClipRect(_clip_min, _clip_max, true);
        _draw->AddText(
            _body_font,
            _body_font->FontSize,
            ImVec2(_text_x, _position.y + 15.0f),
            _text_color,
            _title.data(),
            _title.data() + _title.size());
        _draw->AddText(
            _emphasis_font,
            _emphasis_font->FontSize,
            ImVec2(_text_x, _position.y + 39.0f),
            _secondary_color,
            _artist_album.c_str());
        _draw->PopClipRect();

        _draw->AddText(
            ImVec2(
                _duration_x,
                _position.y + (_card_height - _duration_size.y) * 0.5f),
            _secondary_color,
            _duration.c_str());
        if (!_offline_text.empty()) {
            const ImU32 _offline_color = _offline.state == offline_state::failed
                ? IM_COL32(255, 89, 89, 255)
                : _secondary_color;
            _draw->AddText(
                ImVec2(
                    _offline_x,
                    _position.y + (_card_height - _offline_size.y) * 0.5f),
                _offline_color,
                _offline_text.c_str());
        }
        ImGui::PopID();
        ImGui::PopID();
    }

    void _draw_track_cards(
        context& ctx,
        const std::vector<peer_record>& peers,
        const std::unordered_set<std::string>& available_hashes)
    {
        if (_view._tracks_dirty) {
            _sort_tracks(_view._tracks);
            _view._tracks_dirty = false;
        }
        if (_view._selection_dirty) {
            _restore_selection(ctx, peers);
            _view._selection_dirty = false;
        }

        std::vector<std::size_t> _visible_tracks;
        _visible_tracks.reserve(_view._tracks.size());
        for (std::size_t _index = 0; _index < _view._tracks.size(); ++_index) {
            if (_track_matches_filters(_view._tracks[_index], available_hashes)) {
                _visible_tracks.push_back(_index);
            }
        }

        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2(_library_list_horizontal_padding, 0.0f));
        const bool _list_visible = ImGui::BeginChild(
            "##LibraryCards",
            ImVec2(0.0f, 0.0f),
            false,
            ImGuiWindowFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        if (!_list_visible) {
            ImGui::EndChild();
            return;
        }
        if (_visible_tracks.empty()) {
            ImGui::TextDisabled(
                _view._tracks.empty()
                    ? "No tracks in this library"
                    : "No tracks match the current filters");
        }

        ImGuiListClipper _clipper;
        _clipper.Begin(static_cast<int>(_visible_tracks.size()));
        while (_clipper.Step()) {
            for (int _row = _clipper.DisplayStart; _row < _clipper.DisplayEnd; ++_row) {
                const std::size_t _track_index = _visible_tracks[static_cast<std::size_t>(_row)];
                _draw_track_card(ctx, _view._tracks[_track_index], peers);
            }
        }
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

void draw_library(context& ctx)
{
    const instance_info _instance = ctx.store.instance();
    const std::vector<peer_record> _peers = ctx.store.peers();
    _refresh_tracks(ctx, _instance, _peers);
    _update_scan_message(ctx);

    const float _toolbar_inset = _library_list_horizontal_padding
        + _track_card_padding;
    const ImVec2 _toolbar_position = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(
        _toolbar_position.x + _toolbar_inset,
        _toolbar_position.y + _toolbar_inset));
    const float _toolbar_start_x = ImGui::GetCursorPosX();
    const float _toolbar_width = ImGui::GetContentRegionAvail().x;
    ImGui::Checkbox("Offline only", &_view._offline_only);
    ImGui::SameLine();
    constexpr const char* _search_fields[] = { "Title", "Artist", "Album" };
    int _selected_field = static_cast<int>(_view._field);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::Combo("##SearchField", &_selected_field, _search_fields, 3)) {
        _view._field = static_cast<_search_field>(_selected_field);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##TrackSearch", "Search...", &_view._search);
    ImGui::SameLine();

    const std::unordered_set<std::string> _available = _available_hashes(ctx, _instance.id);
    const std::size_t _visible_track_count = static_cast<std::size_t>(std::count_if(
        _view._tracks.begin(),
        _view._tracks.end(),
        [&_available](const track& value) {
            return _track_matches_filters(value, _available);
        }));
    ImGui::TextDisabled("%zu tracks from %zu libraries", _visible_track_count, _view._catalog_names.size());

    const ImGuiStyle& _style = ImGui::GetStyle();
    const float _friends_width = ImGui::CalcTextSize("Friends").x + _style.FramePadding.x * 2.0f;
    const float _settings_width = ImGui::CalcTextSize("Settings").x + _style.FramePadding.x * 2.0f;
    const float _buttons_width = _friends_width + _style.ItemSpacing.x + _settings_width;
    ImGui::SameLine();
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), _toolbar_start_x + _toolbar_width - _buttons_width));
    if (ImGui::Button("Friends", ImVec2(_friends_width, 0.0f))) {
        open_friends(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Settings", ImVec2(_settings_width, 0.0f))) {
        open_settings(ctx);
    }

    const widget_message* _message = ctx.active_message();
    if (_message == nullptr) {
        ImGui::TextUnformatted(" ");
    } else if (_message->kind == widget_message_kind::error) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
            "%s",
            _message->text.c_str());
    } else {
        ImGui::TextDisabled("%s", _message->text.c_str());
    }

    _draw_track_cards(ctx, _peers, _available);
}

}
