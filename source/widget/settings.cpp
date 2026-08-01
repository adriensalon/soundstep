#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <core/config.hpp>
#include <core/context.hpp>
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
    };

    _settings_dialog _dialog;

    bool _scan_settings_changed(const configuration& config)
    {
        return config.library_path.u8string() != _dialog._library_path
            || config.scan_subdirectories != _dialog._scan_subdirectories;
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
            std::snprintf(
                _text,
                sizeof(_text),
                "%llu B",
                static_cast<unsigned long long>(bytes));
        } else {
            std::snprintf(_text, sizeof(_text), "%.1f %s", _value, _units[_unit]);
        }
        return _text;
    }

    void _draw_library_scan(context& ctx)
    {
        const configuration _config = ctx.store.config();
        const library_scan_status _scan = ctx.scanner.status();
        const bool _scan_running = _scan.state == library_scan_state::scanning;
        const bool _settings_changed = _scan_settings_changed(_config);

        ImGui::BeginDisabled(_scan_running || _config.library_path.empty() || _settings_changed);
        if (ImGui::Button(icons::scan_now, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            if (ctx.try_action([&ctx] { ctx.scanner.scan(); })) {
                ctx.notify("Scanning library...");
            }
        }
        ImGui::EndDisabled();
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
    _dialog._open_requested = true;
}

void draw_settings(context& ctx)
{
    if (_dialog._open_requested) {
        ImGui::OpenPopup("Settings");
        _dialog._open_requested = false;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }

    ImGui::TextDisabled("Device name");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool _name_submitted = ImGui::InputText(
        "###Device name input",
        &_dialog._instance_name,
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (_name_submitted || ImGui::IsItemDeactivatedAfterEdit()) {
        const bool _saved = ctx.try_action([&ctx] {
            if (_dialog._instance_name.empty()) {
                throw widget_error("Device name cannot be empty");
            }
            ctx.store.set_instance_name(_dialog._instance_name);
        });
        if (!_saved) {
            _dialog._instance_name = ctx.store.instance().name;
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Music folder");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool _path_submitted = ImGui::InputText("###Music folder input", &_dialog._library_path, ImGuiInputTextFlags_EnterReturnsTrue);
    if (_path_submitted || ImGui::IsItemDeactivatedAfterEdit()) {
        configuration _config = ctx.store.config();
        _config.library_path = std::filesystem::u8path(_dialog._library_path);
        if (!_save_config(ctx, _config, true)) {
            _restore_config_fields(ctx);
        }
    }
    ImGui::TextDisabled("The folder whose supported audio files belong to this device");
	if (ImGui::Checkbox("Include subfolders", &_dialog._scan_subdirectories)) {
        configuration _config = ctx.store.config();
        const bool _path_is_committed = _config.library_path.u8string() == _dialog._library_path;
        _config.scan_subdirectories = _dialog._scan_subdirectories;
        if (!_save_config(ctx, _config, _path_is_committed)) {
            _restore_config_fields(ctx);
        }
    }
    if (ImGui::Checkbox("Rescan when SoundStep starts", &_dialog._scan_on_startup)) {
        configuration _config = ctx.store.config();
        _config.scan_on_startup = _dialog._scan_on_startup;
        if (!_save_config(ctx, _config, false)) {
            _restore_config_fields(ctx);
        }
    }
    ImGui::Spacing();
    _draw_library_scan(ctx);

    ImGui::Spacing();
    ImGui::TextDisabled("Storage");
    const music_storage_usage _usage = ctx.store.music_usage();
    const std::string _local_usage = _storage_size_text(_usage.local_bytes);
    const std::string _downloaded_usage = _storage_size_text(_usage.downloaded_bytes);
    ImGui::Text("Local music: %s", _local_usage.c_str());
    ImGui::Text("Downloaded music: %s", _downloaded_usage.c_str());

    ImGui::Spacing();
    ImGui::TextDisabled("Network");
    if (ImGui::Checkbox("Enable automatic discovery on LAN", &_dialog._lan_discovery_enabled)) {
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
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

}
