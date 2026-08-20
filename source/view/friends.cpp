#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <im_anim.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <core/context.hpp>
#include <view/animation.hpp>
#include <view/friends.hpp>
#include <view/icons.hpp>
#include <view/tooltip.hpp>

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
        bool _closing { false };
        bool _pairing_error { false };
    };

    _friends_dialog _dialog;

    ImVec2 _main_work_center()
    {
        const ImGuiViewport* _viewport = ImGui::GetMainViewport();
        return ImVec2(
            _viewport->WorkPos.x + _viewport->WorkSize.x * 0.5f,
            _viewport->WorkPos.y + _viewport->WorkSize.y * 0.5f);
    }

    std::uint64_t current_time_ms()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
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
        return value.family == peer_endpoint_family::ipv6 ? "[" + value.host + "]:" + _port : value.host + ":" + _port;
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

        const std::uint64_t _now = current_time_ms();
        for (const peer_record& _record : peers) {
            ImGui::PushID(_record.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool _selected = _dialog._selected_id == _record.id;
            if (ImGui::Selectable(_record.name.c_str(), _selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                _dialog._selected_id = _record.id;
            }
            if (ImGui::IsItemHovered()) {
                draw_tooltip(_record.id.c_str());
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
            if (animation_toggle("##LibraryEnabled", &_library_enabled, true)) {
                const std::string _peer_id = _record.id;
                ctx.try_action([&ctx, _peer_id, _library_enabled] {
                    ctx.store.set_peer_library_enabled(_peer_id, _library_enabled);
                });
            }
            if (ImGui::IsItemHovered()) {
                draw_tooltip("Show this friend's tracks in the unified library");
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
        ImGui::SetNextWindowPos(_main_work_center(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("RemoveFriend", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
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
    _dialog._closing = false;
    _dialog._pairing_error = false;
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

    const bool _popup_open = ImGui::IsPopupOpen("Friends");
    const ImGuiID _dialog_motion_owner = ImGui::GetID("##FriendsDialogMotion");
    const float _dialog_reveal = animation_tween(_dialog_motion_owner, 0x31001u, _popup_open && !_dialog._closing ? 1.0f : 0.0f, animation_normal, iam_ease_out_cubic);
    const float _dialog_scale = 0.97f + 0.03f * _dialog_reveal;
    ImGui::SetNextWindowSize(ImVec2(760.0f * _dialog_scale, 0.0f), ImGuiCond_Always);
    const ImVec2 _dialog_center = _main_work_center();
    ImGui::SetNextWindowPos(ImVec2(_dialog_center.x, _dialog_center.y + (1.0f - _dialog_reveal) * 10.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(ImGui::GetStyleColorVec4(ImGuiCol_PopupBg).w * (0.72f + 0.28f * _dialog_reveal));
    if (!ImGui::BeginPopupModal("Friends", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (std::max)(0.02f, _dialog_reveal));

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
        const float _copy_button_width = ImGui::CalcTextSize(icons::copy_sharing_code).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float _invite_input_width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - _copy_button_width);
        ImGui::SetNextItemWidth(_invite_input_width);
        ImGui::InputText("##own_pairing_code", &_dialog._own_invite, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button(icons::copy_sharing_code, ImVec2(_copy_button_width, 0.0f))) {
            ImGui::SetClipboardText(_dialog._own_invite.c_str());
            ctx.notify("Sharing code copied.");
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Add friend");
    ImGui::Spacing();
    const float _pair_button_width = ImGui::CalcTextSize(icons::pair).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float _pairing_input_width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - _pair_button_width);
    ImGui::SetNextItemWidth(_pairing_input_width);
    const ImGuiID _pairing_error_owner = ImGui::GetID("##PairingCodeError");
    const float _pairing_shake = iam_shake(_pairing_error_owner, 7.0f, 28.0f, 0.38f, ImGui::GetIO().DeltaTime);
    const ImVec2 _pairing_position = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(_pairing_position.x + _pairing_shake, _pairing_position.y));
    const float _pairing_error_reveal = animation_tween(_pairing_error_owner, 0x31002u, _dialog._pairing_error ? 1.0f : 0.0f, animation_quick, iam_ease_out_cubic);
    const ImVec4 _pairing_background = iam_get_blended_color(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg), ImVec4(0.42f, 0.12f, 0.12f, 1.0f), _pairing_error_reveal, iam_col_oklab);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, _pairing_background);
    ImGui::InputText("##friend_pairing_code", &_dialog._pairing_code);
    ImGui::PopStyleColor();
    if (ImGui::IsItemEdited()) {
        _dialog._pairing_error = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(icons::pair, ImVec2(_pair_button_width, 0.0f))) {
        const bool _paired = ctx.try_action([&ctx] {
            if (_dialog._pairing_code.empty()) {
                throw widget_error("Pairing code cannot be empty");
            }
            ctx.network.accept_pairing_code(_dialog._pairing_code);
            _dialog._pairing_code.clear();
        });
        if (_paired) {
            _dialog._pairing_error = false;
            ctx.notify("Friend paired.");
        } else {
            _dialog._pairing_error = true;
            iam_trigger_shake(_pairing_error_owner);
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();
    if (ImGui::Button(icons::close, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
        _dialog._closing = true;
    }
    if (_dialog._closing && _dialog_reveal <= 0.02f) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

}
