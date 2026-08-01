#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <core/context.hpp>
#include <widget/friends.hpp>
#include <widget/icons.hpp>

namespace soundstep {
namespace {

    constexpr std::uint64_t _available_interval_ms = 90'000;

    struct _friends_dialog {
        std::string _selected_id;
        std::string _own_invite;
        std::string _pairing_code;
        std::string _remove_id;
        std::string _remove_name;
        bool _open_requested { false };
    };

    _friends_dialog _dialog;

    std::uint64_t _current_time_ms()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    std::uint64_t _age_ms(const peer_record& value, std::uint64_t now)
    {
        return value.last_seen_ms >= now ? 0 : now - value.last_seen_ms;
    }

    bool _available(const peer_record& value, std::uint64_t now)
    {
        return value.last_seen_ms != 0 && _age_ms(value, now) <= _available_interval_ms;
    }

    std::string _last_seen_text(const peer_record& value, std::uint64_t now)
    {
        if (value.last_seen_ms == 0) {
            return "Never";
        }

        const std::uint64_t _seconds = _age_ms(value, now) / 1'000;
        if (_seconds < 5) {
            return "Just now";
        }
        if (_seconds < 60) {
            return std::to_string(_seconds) + " seconds ago";
        }
        const std::uint64_t _minutes = _seconds / 60;
        if (_minutes < 60) {
            return std::to_string(_minutes) + (_minutes == 1 ? " minute ago" : " minutes ago");
        }
        const std::uint64_t _hours = _minutes / 60;
        if (_hours < 24) {
            return std::to_string(_hours) + (_hours == 1 ? " hour ago" : " hours ago");
        }
        const std::uint64_t _days = _hours / 24;
        return std::to_string(_days) + (_days == 1 ? " day ago" : " days ago");
    }

    std::string _endpoint_text(const peer_endpoint& value)
    {
        const std::string _port = std::to_string(value.port);
        return value.family == peer_endpoint_family::ipv6
            ? "[" + value.host + "]:" + _port
            : value.host + ":" + _port;
    }

    const peer_record* _selected_friend(const std::vector<peer_record>& peers)
    {
        for (const peer_record& _record : peers) {
            if (_record.id == _dialog._selected_id) {
                return &_record;
            }
        }
        return nullptr;
    }

    void _draw_friend_table(context& ctx, const std::vector<peer_record>& peers)
    {
        constexpr ImGuiTableFlags _flags = ImGuiTableFlags_Borders
            | ImGuiTableFlags_RowBg
            | ImGuiTableFlags_Resizable
            | ImGuiTableFlags_ScrollY
            | ImGuiTableFlags_SizingStretchProp;
        constexpr ImVec4 _transparent(0.0f, 0.0f, 0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, _transparent);
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, _transparent);
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, _transparent);
        if (!ImGui::BeginTable("##Friends", 6, _flags, ImVec2(0.0f, 240.0f))) {
            ImGui::PopStyleColor(3);
            return;
        }

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Endpoint", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Paired through", ImGuiTableColumnFlags_WidthFixed, 105.0f);
        ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed, 125.0f);
        ImGui::TableHeadersRow();

        if (peers.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("No friends yet");
        }

        const std::uint64_t _now = _current_time_ms();
        for (const peer_record& _record : peers) {
            ImGui::PushID(_record.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool _selected = _dialog._selected_id == _record.id;
            if (ImGui::Selectable(
                    _record.name.c_str(),
                    _selected,
                    ImGuiSelectableFlags_SpanAllColumns
                        | ImGuiSelectableFlags_AllowOverlap)) {
                _dialog._selected_id = _record.id;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", _record.id.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            if (_record.fingerprint.empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "Pair again");
            } else if (_available(_record, _now)) {
                ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f), "Available");
            } else {
                ImGui::TextDisabled("Offline");
            }

            ImGui::TableSetColumnIndex(2);
            bool _library_enabled = _record.library_enabled;
            if (ImGui::Checkbox("##LibraryEnabled", &_library_enabled)) {
                const std::string _peer_id = _record.id;
                ctx.try_action([&ctx, _peer_id, _library_enabled] {
                    ctx.store.set_peer_library_enabled(_peer_id, _library_enabled);
                });
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show this friend's tracks in the unified library");
            }

            ImGui::TableSetColumnIndex(3);
            if (_record.endpoints.empty()) {
                ImGui::TextDisabled("Unknown");
            } else {
                const peer_endpoint& _preferred = _record.endpoints.front();
                const std::string _endpoint = _endpoint_text(_preferred);
                ImGui::TextUnformatted(_endpoint.c_str());
                if (_record.endpoints.size() > 1) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("+%zu", _record.endpoints.size() - 1);
                }
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(_record.origin == peer_origin::lan ? "Local network" : "Pairing code");
            ImGui::TableSetColumnIndex(5);
            const std::string _last_seen = _last_seen_text(_record, _now);
            ImGui::TextUnformatted(_last_seen.c_str());
            ImGui::PopID();
        }

        ImGui::EndTable();
        ImGui::PopStyleColor(3);
    }

    void _draw_remove_confirmation(context& ctx)
    {
        ImGui::SetNextWindowPos(
            ImGui::GetMainViewport()->GetCenter(),
            ImGuiCond_Always,
            ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(
                "RemoveFriend",
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            return;
        }

        ImGui::Text("Remove %s?", _dialog._remove_name.c_str());
        ImGui::TextDisabled("Its cached catalog will also be removed.");
        const float _button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button(icons::cancel, ImVec2(_button_width, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(icons::remove, ImVec2(_button_width, 0.0f))) {
            const bool _removed = ctx.try_action([&ctx] {
                ctx.store.remove_peer(_dialog._remove_id);
            });
            if (_removed) {
                _dialog._selected_id.clear();
                _dialog._remove_id.clear();
                _dialog._remove_name.clear();
                ctx.notify("Friend removed.");
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

}

void open_friends(context& ctx)
{
    _dialog._pairing_code.clear();
    _dialog._own_invite.clear();
    _dialog._remove_id.clear();
    _dialog._remove_name.clear();
    ctx.try_action([&ctx] {
        _dialog._own_invite = ctx.network.create_pairing_code();
    });
    _dialog._open_requested = true;
}

void draw_friends(context& ctx)
{
    if (_dialog._open_requested) {
        ImGui::OpenPopup("Friends");
        _dialog._open_requested = false;
    }

    ImGui::SetNextWindowSize(ImVec2(760.0f, 620.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImGui::GetMainViewport()->GetCenter(),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Friends", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }

    std::vector<peer_record> _peers = ctx.store.peers();
    if (_selected_friend(_peers) == nullptr) {
        _dialog._selected_id.clear();
    }

    _draw_friend_table(ctx, _peers);
    const peer_record* _selected = _selected_friend(_peers);

    const float _friend_button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(icons::refresh, ImVec2(_friend_button_width, 0.0f))) {
        ctx.network.refresh_catalogs_async();
        ctx.notify("Refreshing friend libraries...");
    }
    ImGui::SameLine();
    const bool _automatic_friend = _selected != nullptr && _selected->origin == peer_origin::lan;
    ImGui::BeginDisabled(_selected == nullptr || _automatic_friend);
    if (ImGui::Button(icons::remove, ImVec2(_friend_button_width, 0.0f))) {
        _dialog._remove_id = _selected->id;
        _dialog._remove_name = _selected->name;
        ImGui::OpenPopup("RemoveFriend");
    }
    ImGui::EndDisabled();
    if (_automatic_friend) {
        ImGui::TextDisabled("Local-network friends are managed automatically.");
    }
    _draw_remove_confirmation(ctx);

    ImGui::Spacing();
    ImGui::TextDisabled("Share this instance");
    ImGui::Spacing();
    if (!_dialog._own_invite.empty()) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText(
            "##own_pairing_code",
            &_dialog._own_invite,
            ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button(
                icons::copy_sharing_code,
                ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            ImGui::SetClipboardText(_dialog._own_invite.c_str());
            ctx.notify("Sharing code copied.");
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Add friend");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##friend_pairing_code", &_dialog._pairing_code);
    if (ImGui::Button(icons::pair, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
        const bool _paired = ctx.try_action([&ctx] {
            if (_dialog._pairing_code.empty()) {
                throw widget_error("Pairing code cannot be empty");
            }
            ctx.network.accept_pairing_code(_dialog._pairing_code);
            _dialog._pairing_code.clear();
        });
        if (_paired) {
            ctx.notify("Friend paired.");
        }
    }

    ImGui::Spacing();
    if (ImGui::Button(icons::close, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

}
