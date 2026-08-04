#include <algorithm>

#ifndef __ANDROID__
#include <GLFW/glfw3.h>
#endif
#include <imgui.h>

#include <core/context.hpp>
#include <view/animation.hpp>
#include <view/app.hpp>
#include <view/friends.hpp>
#include <view/library.hpp>
#include <view/player.hpp>
#include <view/settings.hpp>
#include <view/tooltip.hpp>

namespace soundstep {
namespace {

    ImVec4 _chrome_background()
    {
        return ImVec4(0.025f, 0.025f, 0.025f, 0.82f);
    }

#ifndef __ANDROID__
    constexpr float _title_bar_height = 44.0f;
    constexpr float _window_button_width = 46.0f;
    constexpr float _title_horizontal_inset = 29.0f;
    constexpr float _title_vertical_inset = 20.0f;

    enum struct _window_button_kind {
        minimize,
        maximize,
        restore,
        close
    };

    bool _draw_window_button(const char* id, const char* tooltip, _window_button_kind kind, ImVec2 position)
    {
        ImGui::SetCursorScreenPos(position);
        ImGui::InvisibleButton(id, ImVec2(_window_button_width, _title_bar_height));
        const bool _clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const bool _hovered = ImGui::IsItemHovered();
        const bool _active = ImGui::IsItemActive();
        const ImGuiID _owner = ImGui::GetItemID();
        const float _hover = animation_tween(_owner, 0x81001u, _hovered ? 1.0f : 0.0f, animation_quick);

        ImVec4 _background = kind == _window_button_kind::close ? ImVec4(0.86f, 0.16f, 0.18f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
        if (_active) {
            _background.x *= 0.78f;
            _background.y *= 0.78f;
            _background.z *= 0.78f;
        }

        ImDrawList* _draw = ImGui::GetWindowDrawList();
        _draw->AddRectFilled(position, ImVec2(position.x + _window_button_width, position.y + _title_bar_height), animation_with_alpha(_background, _hover));

        const ImU32 _glyph_color = ImGui::GetColorU32(ImGuiCol_Text);
        const ImVec2 _center(position.x + _window_button_width * 0.5f, position.y + _title_bar_height * 0.5f);
        constexpr float _half_size = 5.0f;
        constexpr float _stroke = 1.25f;
        switch (kind) {
        case _window_button_kind::minimize:
            _draw->AddLine(ImVec2(_center.x - _half_size, _center.y + 3.0f), ImVec2(_center.x + _half_size, _center.y + 3.0f), _glyph_color, _stroke);
            break;
        case _window_button_kind::maximize:
            _draw->AddRect(ImVec2(_center.x - _half_size, _center.y - _half_size), ImVec2(_center.x + _half_size, _center.y + _half_size), _glyph_color, 0.0f, 0, _stroke);
            break;
        case _window_button_kind::restore:
            _draw->AddRect(ImVec2(_center.x - 3.0f, _center.y - _half_size), ImVec2(_center.x + _half_size, _center.y + 3.0f), _glyph_color, 0.0f, 0, _stroke);
            _draw->AddRect(ImVec2(_center.x - _half_size, _center.y - 3.0f), ImVec2(_center.x + 3.0f, _center.y + _half_size), _glyph_color, 0.0f, 0, _stroke);
            break;
        case _window_button_kind::close:
            _draw->AddLine(ImVec2(_center.x - _half_size, _center.y - _half_size), ImVec2(_center.x + _half_size, _center.y + _half_size), _glyph_color, _stroke);
            _draw->AddLine(ImVec2(_center.x + _half_size, _center.y - _half_size), ImVec2(_center.x - _half_size, _center.y + _half_size), _glyph_color, _stroke);
            break;
        }

        if (_hovered) {
            draw_tooltip(tooltip);
        }
        return _clicked;
    }

    void _draw_title_bar(const context& ctx, GLFWwindow* window)
    {
        const ImVec2 _position = ImGui::GetCursorScreenPos();
        const float _width = ImGui::GetContentRegionAvail().x;
        const float _buttons_width = _window_button_width * 3.0f;
        const float _drag_width = (std::max)(0.0f, _width - _buttons_width);
        ImDrawList* _draw = ImGui::GetWindowDrawList();

        _draw->AddRectFilled(_position, ImVec2(_position.x + _width, _position.y + _title_bar_height), ImGui::GetColorU32(_chrome_background()));

        ImGui::SetCursorScreenPos(_position);
        ImGui::InvisibleButton("##WindowDrag", ImVec2(_drag_width, _title_bar_height));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE) {
                glfwRestoreWindow(window);
            } else {
                glfwMaximizeWindow(window);
            }
        } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            glfwStartWindowMove(window);
        }

        const char* _title = "SOUNDSTEP";
        ImFont* _title_font = ctx.fonts.brand != nullptr ? ctx.fonts.brand : ImGui::GetFont();
        _draw->AddText(_title_font, _title_font->FontSize, ImVec2(_position.x + _title_horizontal_inset, _position.y + _title_vertical_inset), ImGui::GetColorU32(ImGuiCol_Text), _title);

        const float _buttons_x = _position.x + _width - _buttons_width;
        if (_draw_window_button("##WindowMinimize", "Minimize", _window_button_kind::minimize, ImVec2(_buttons_x, _position.y))) {
            glfwIconifyWindow(window);
        }

        const bool _maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE;
        if (_draw_window_button("##WindowMaximize", _maximized ? "Restore" : "Maximize", _maximized ? _window_button_kind::restore : _window_button_kind::maximize, ImVec2(_buttons_x + _window_button_width, _position.y))) {
            if (_maximized) {
                glfwRestoreWindow(window);
            } else {
                glfwMaximizeWindow(window);
            }
        }

        if (_draw_window_button("##WindowClose", "Close", _window_button_kind::close, ImVec2(_buttons_x + _window_button_width * 2.0f, _position.y))) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        ImGui::SetCursorScreenPos(_position);
        ImGui::Dummy(ImVec2(_width, _title_bar_height));
        ImGui::SetCursorScreenPos(ImVec2(_position.x, _position.y + _title_bar_height));
    }
#endif

}

void draw_app(context& ctx, GLFWwindow* window)
{
    const ImGuiViewport* _viewport = ImGui::GetMainViewport();
    const ImVec2 _window_padding = ImGui::GetStyle().WindowPadding;
    ImGui::SetNextWindowPos(_viewport->WorkPos);
    ImGui::SetNextWindowSize(_viewport->WorkSize);
    constexpr ImGuiWindowFlags _window_flags = ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 6.0f));
    if (ImGui::Begin("##SoundstepMain", nullptr, _window_flags)) {
#ifndef __ANDROID__
        _draw_title_bar(ctx, window);
#else
        static_cast<void>(window);
#endif
        constexpr float _player_height = 112.0f;
        const float _library_player_gap = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
        const float _content_start_y = ImGui::GetCursorPosY();
        const float _content_height = (std::max)(0.0f, ImGui::GetContentRegionAvail().y - _player_height - _library_player_gap);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(_window_padding.x, 0.0f));
        if (ImGui::BeginChild("##SoundstepContent", ImVec2(0.0f, _content_height), false, ImGuiWindowFlags_AlwaysUseWindowPadding)) {
            draw_library(ctx);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        const ImVec2 _lower_chrome_min(_viewport->WorkPos.x, _viewport->WorkPos.y + _content_start_y + _content_height);
        const ImVec2 _lower_chrome_max(_viewport->WorkPos.x + _viewport->WorkSize.x, _viewport->WorkPos.y + _viewport->WorkSize.y);
        ImGui::GetWindowDrawList()->AddRectFilled(_lower_chrome_min, _lower_chrome_max, ImGui::GetColorU32(_chrome_background()));

        ImGui::SetCursorPosY(_content_start_y + _content_height + _library_player_gap);
        if (ImGui::BeginChild("##SoundstepPlayer", ImVec2(0.0f, 0.0f), false)) {
            draw_player(ctx);
        }
        ImGui::EndChild();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, _window_padding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ImGui::GetStyle().FrameRounding);
        draw_friends(ctx);
        draw_settings(ctx);
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
}

}
