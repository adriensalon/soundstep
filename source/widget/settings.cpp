#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <im_anim.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <core/config.hpp>
#include <core/context.hpp>
#include <widget/animation.hpp>
#include <widget/icons.hpp>
#include <widget/settings.hpp>

namespace soundstep {
namespace {

    struct _settings_dialog {
        std::string _instance_name;
        std::string _library_path;
        bool _scan_subdirectories { true };
        bool _scan_on_startup { true };
        bool _lan_discovery_enabled { true };
        bool _open_requested { false };
        bool _closing { false };
        bool _name_error { false };
        bool _path_error { false };
    };

    _settings_dialog _dialog;
    constexpr const char* _scan_button_label = "Scan now";

    ImVec2 _main_work_center()
    {
        const ImGuiViewport* _viewport = ImGui::GetMainViewport();
        return ImVec2(
            _viewport->WorkPos.x + _viewport->WorkSize.x * 0.5f,
            _viewport->WorkPos.y + _viewport->WorkSize.y * 0.5f);
    }

    bool _scan_settings_changed(const configuration& config)
    {
        return config.library_path.u8string() != _dialog._library_path || config.scan_subdirectories != _dialog._scan_subdirectories;
    }

    bool _save_config(context& ctx, const configuration& config, bool rescan)
    {
        if (!ctx.try_action([&ctx, &config] { ctx.store.set_config(config); })) {
            return false;
        }
        if (rescan
            && !config.library_path.empty()
            && ctx.scanner.status().state != library_scan_state::scanning
            && ctx.try_action([&ctx] { ctx.scanner.scan(); })) {
            ctx.notify("Scanning library...");
        }
        return true;
    }

    void _restore_config_fields(context& ctx)
    {
        const configuration _config = ctx.store.config();
        _dialog._library_path = _config.library_path.u8string();
        _dialog._scan_subdirectories = _config.scan_subdirectories;
        _dialog._scan_on_startup = _config.scan_on_startup;
        _dialog._lan_discovery_enabled = _config.lan_discovery_enabled;
    }

    std::string _storage_size_text(std::uint64_t bytes)
    {
        constexpr const char* _units[] = { "B", "KB", "MB", "GB", "TB" };
        double _value = static_cast<double>(bytes);
        std::size_t _unit = 0;
        while (_value >= 1024.0 && _unit + 1 < 5) {
            _value /= 1024.0;
            ++_unit;
        }

        char _text[32] { };
        if (_unit == 0) {
            std::snprintf(_text, sizeof(_text), "%llu B", static_cast<unsigned long long>(bytes));
        } else {
            std::snprintf(_text, sizeof(_text), "%.1f %s", _value, _units[_unit]);
        }
        return _text;
    }

    void _draw_library_scan(context& ctx, float button_width)
    {
        const configuration _config = ctx.store.config();
        const library_scan_status _scan = ctx.scanner.status();
        const bool _scan_running = _scan.state == library_scan_state::scanning;
        const bool _settings_changed = _scan_settings_changed(_config);

        const ImVec2 _button_position = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(_scan_running || _config.library_path.empty() || _settings_changed);
        if (ImGui::Button(_scan_button_label, ImVec2(button_width, 0.0f))) {
            if (ctx.try_action([&ctx] { ctx.scanner.scan(); })) {
                ctx.notify("Scanning library...");
            }
        }
        ImGui::EndDisabled();
        if (_scan_running) {
            animation_activity_indicator(
                ImGui::GetID("##ScanActivity"),
                ImVec2(_button_position.x + button_width - ImGui::GetFrameHeight() * 0.55f, _button_position.y + ImGui::GetFrameHeight() * 0.5f),
                5.0f,
                ImGui::GetColorU32(ImGuiCol_TextDisabled));
        }
    }

}

void open_settings(context& ctx)
{
    const instance_info _instance = ctx.store.instance();
    const configuration _config = ctx.store.config();
    _dialog._instance_name = _instance.name;
    _dialog._library_path = _config.library_path.u8string();
    _dialog._scan_subdirectories = _config.scan_subdirectories;
    _dialog._scan_on_startup = _config.scan_on_startup;
    _dialog._lan_discovery_enabled = _config.lan_discovery_enabled;
    _dialog._closing = false;
    _dialog._name_error = false;
    _dialog._path_error = false;
    _dialog._open_requested = true;
}

void draw_settings(context& ctx)
{
    if (_dialog._open_requested) {
        ImGui::OpenPopup("Settings");
        _dialog._open_requested = false;
    }

    const bool _popup_open = ImGui::IsPopupOpen("Settings");
    const ImGuiID _dialog_motion_owner = ImGui::GetID("##SettingsDialogMotion");
    const float _dialog_reveal = animation_tween(_dialog_motion_owner, 0x32001u, _popup_open && !_dialog._closing ? 1.0f : 0.0f, animation_normal, iam_ease_out_cubic);
    const float _dialog_scale = 0.97f + 0.03f * _dialog_reveal;
    ImGui::SetNextWindowSize(ImVec2(560.0f * _dialog_scale, 0.0f), ImGuiCond_Always);
    const ImVec2 _dialog_center = _main_work_center();
    ImGui::SetNextWindowPos(ImVec2(_dialog_center.x, _dialog_center.y + (1.0f - _dialog_reveal) * 10.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(ImGui::GetStyleColorVec4(ImGuiCol_PopupBg).w * (0.72f + 0.28f * _dialog_reveal));
    if (!ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (std::max)(0.02f, _dialog_reveal));

    ImGui::TextDisabled("Device name");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const ImGuiID _name_error_owner = ImGui::GetID("##DeviceNameError");
    const float _name_shake = iam_shake(_name_error_owner, 7.0f, 28.0f, 0.38f, ImGui::GetIO().DeltaTime);
    const ImVec2 _name_position = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(_name_position.x + _name_shake, _name_position.y));
    const float _name_error_reveal = animation_tween(_name_error_owner, 0x32002u, _dialog._name_error ? 1.0f : 0.0f, animation_quick, iam_ease_out_cubic);
    const ImVec4 _name_background = iam_get_blended_color(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg), ImVec4(0.42f, 0.12f, 0.12f, 1.0f), _name_error_reveal, iam_col_oklab);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, _name_background);
    const bool _name_submitted = ImGui::InputText("###Device name input", &_dialog._instance_name, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    if (ImGui::IsItemEdited()) {
        _dialog._name_error = false;
    }
    if (_name_submitted || ImGui::IsItemDeactivatedAfterEdit()) {
        const bool _saved = ctx.try_action([&ctx] {
            if (_dialog._instance_name.empty()) {
                throw widget_error("Device name cannot be empty");
            }
            ctx.store.set_instance_name(_dialog._instance_name);
        });
        if (!_saved) {
            _dialog._name_error = true;
            iam_trigger_shake(_name_error_owner);
            _dialog._instance_name = ctx.store.instance().name;
        } else {
            _dialog._name_error = false;
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Music folder");
    const float _scan_button_width = ImGui::CalcTextSize(_scan_button_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float _path_width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - _scan_button_width);
    ImGui::SetNextItemWidth(_path_width);
    const ImGuiID _path_error_owner = ImGui::GetID("##MusicPathError");
    const float _path_shake = iam_shake(_path_error_owner, 7.0f, 28.0f, 0.38f, ImGui::GetIO().DeltaTime);
    const ImVec2 _path_position = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(_path_position.x + _path_shake, _path_position.y));
    const float _path_error_reveal = animation_tween(_path_error_owner, 0x32003u, _dialog._path_error ? 1.0f : 0.0f, animation_quick, iam_ease_out_cubic);
    const ImVec4 _path_background = iam_get_blended_color(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg), ImVec4(0.42f, 0.12f, 0.12f, 1.0f), _path_error_reveal, iam_col_oklab);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, _path_background);
    const bool _path_submitted = ImGui::InputText("###Music folder input", &_dialog._library_path, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    if (ImGui::IsItemEdited()) {
        _dialog._path_error = false;
    }
    if (_path_submitted || ImGui::IsItemDeactivatedAfterEdit()) {
        configuration _config = ctx.store.config();
        _config.library_path = std::filesystem::u8path(_dialog._library_path);
        if (!_save_config(ctx, _config, true)) {
            _dialog._path_error = true;
            iam_trigger_shake(_path_error_owner);
            _restore_config_fields(ctx);
        } else {
            _dialog._path_error = false;
        }
    }
    ImGui::SameLine();
    _draw_library_scan(ctx, _scan_button_width);
    ImGui::TextDisabled("The folder whose supported audio files belong to this device");
    if (animation_toggle("Include subfolders", &_dialog._scan_subdirectories)) {
        configuration _config = ctx.store.config();
        const bool _path_is_committed = _config.library_path.u8string() == _dialog._library_path;
        _config.scan_subdirectories = _dialog._scan_subdirectories;
        if (!_save_config(ctx, _config, _path_is_committed)) {
            _restore_config_fields(ctx);
        }
    }
    if (animation_toggle("Rescan when SoundStep starts", &_dialog._scan_on_startup)) {
        configuration _config = ctx.store.config();
        _config.scan_on_startup = _dialog._scan_on_startup;
        if (!_save_config(ctx, _config, false)) {
            _restore_config_fields(ctx);
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Storage");
    const music_storage_usage _usage = ctx.store.music_usage();
    const std::string _local_usage = _storage_size_text(_usage.local_bytes);
    const std::string _downloaded_usage = _storage_size_text(_usage.downloaded_bytes);
    ImGui::Text("Local music: %s", _local_usage.c_str());
    ImGui::Text("Downloaded music: %s", _downloaded_usage.c_str());

    ImGui::Spacing();
    ImGui::TextDisabled("Network");
    if (animation_toggle("Enable automatic discovery on LAN", &_dialog._lan_discovery_enabled)) {
        configuration _config = ctx.store.config();
        _config.lan_discovery_enabled = _dialog._lan_discovery_enabled;
        if (!_save_config(ctx, _config, false)) {
            _restore_config_fields(ctx);
        }
    }
    ImGui::TextDisabled("Other advertising Soundstep instances remain visible when this is disabled");

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
