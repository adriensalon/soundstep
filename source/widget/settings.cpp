#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <core/config.hpp>
#include <core/context.hpp>
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

        ImGui::BeginDisabled(
            _scan_running
            || _config.library_path.empty()
            || _settings_changed);
        if (ImGui::Button(
                "Scan now",
                ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            if (ctx.try_action([&ctx] { ctx.scanner.scan(); })) {
                ctx.notify("Scanning library...");
            }
        }
        ImGui::EndDisabled();

        if (_settings_changed) {
            ImGui::TextDisabled("Save library changes before scanning.");
        } else if (_config.library_path.empty()) {
            ImGui::TextDisabled("No music folder configured.");
        } else if (!_scan_running && _scan.state == library_scan_state::idle) {
            ImGui::TextDisabled("Scan the configured music folder.");
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
    _dialog._open_requested = true;
}

void draw_settings(context& ctx)
{
    if (_dialog._open_requested) {
        ImGui::OpenPopup("Settings");
        _dialog._open_requested = false;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImGui::GetMainViewport()->GetCenter(),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(
            "Settings",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }

    ImGui::TextUnformatted("General");
    ImGui::SetNextItemWidth(520.0f);
    ImGui::InputText("Instance name", &_dialog._instance_name);

    ImGui::Spacing();
    ImGui::SeparatorText("Library");
    ImGui::SetNextItemWidth(520.0f);
    ImGui::InputText("Music folder", &_dialog._library_path);
    ImGui::TextDisabled("The folder whose supported audio files belong to this instance.");
    ImGui::Checkbox("Include subfolders", &_dialog._scan_subdirectories);
    ImGui::Checkbox("Scan when Soundstep starts", &_dialog._scan_on_startup);
    ImGui::Spacing();
    _draw_library_scan(ctx);

    ImGui::Spacing();
    ImGui::SeparatorText("Storage");
    const music_storage_usage _usage = ctx.store.music_usage();
    const std::string _local_usage = _storage_size_text(_usage.local_bytes);
    const std::string _downloaded_usage = _storage_size_text(_usage.downloaded_bytes);
    ImGui::Text("Local music: %s", _local_usage.c_str());
    ImGui::Text("Downloaded music: %s", _downloaded_usage.c_str());

    ImGui::Spacing();
    ImGui::SeparatorText("Network");
    ImGui::Checkbox("Enable automatic discovery on LAN", &_dialog._lan_discovery_enabled);
    ImGui::TextDisabled("Other advertising Soundstep instances remain visible when this is disabled.");

    ImGui::Spacing();
    const float _button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Cancel", ImVec2(_button_width, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(_button_width, 0.0f))) {
        bool _scan_started = false;
        const bool _saved = ctx.try_action([&ctx, &_scan_started] {
            if (_dialog._instance_name.empty()) {
                throw widget_error("Instance name cannot be empty");
            }
            const configuration _previous = ctx.store.config();
            const configuration _updated {
                std::filesystem::u8path(_dialog._library_path),
                _dialog._scan_subdirectories,
                _dialog._scan_on_startup,
                _dialog._lan_discovery_enabled
            };
            ctx.store.set_config(_updated);

            if (ctx.store.instance().name != _dialog._instance_name) {
                ctx.store.set_instance_name(_dialog._instance_name);
            }

            const bool _scan_changed = _previous.library_path != _updated.library_path || _previous.scan_subdirectories != _updated.scan_subdirectories;
            if (_scan_changed && !_updated.library_path.empty()) {
                ctx.scanner.scan();
                _scan_started = true;
            }
        });
        if (_saved) {
            ctx.notify(_scan_started ? "Settings saved. Scanning library..." : "Settings saved.");
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::EndPopup();
}

}
