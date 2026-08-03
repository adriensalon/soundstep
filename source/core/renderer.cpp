#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#ifdef __ANDROID__
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <backends/imgui_impl_android.h>
#else
#define GLFW_INCLUDE_NONE
#define GLAD_GL_IMPLEMENTATION
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <glad/gl.h>
#endif
#include <backends/imgui_impl_opengl3.h>
#include <cmrc/cmrc.hpp>
#include <im_anim.h>

#include <core/playback.hpp>
#include <core/renderer.hpp>

CMRC_DECLARE(soundstep_resource);

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace soundstep {
namespace {

#ifdef __ANDROID__
#define SOUNDSTEP_GLSL_VERSION "#version 300 es\n"
#define SOUNDSTEP_GLSL_PRECISION "precision highp float;\n"
#else
#define SOUNDSTEP_GLSL_VERSION "#version 330 core\n"
#define SOUNDSTEP_GLSL_PRECISION ""
#endif

    constexpr const char* _background_vertex_shader = SOUNDSTEP_GLSL_VERSION R"glsl(

out vec2 vertex_uv;

void main()
{
    vec2 position;
    if (gl_VertexID == 0) {
        position = vec2(-1.0, -1.0);
    } else if (gl_VertexID == 1) {
        position = vec2(3.0, -1.0);
    } else {
        position = vec2(-1.0, 3.0);
    }
    vertex_uv = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)glsl";

    constexpr const char* _background_fragment_shader = SOUNDSTEP_GLSL_VERSION SOUNDSTEP_GLSL_PRECISION R"glsl(

in vec2 vertex_uv;
out vec4 fragment_color;

uniform vec2 resolution;
uniform float visual_time;
uniform float activity;
uniform float rms_level;
uniform float peak_level;
uniform float bass_level;
uniform float mid_level;
uniform float treble_level;
uniform float beat;
uniform float beat_age;

float hash21(vec2 value)
{
    value = fract(value * vec2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return fract(value.x * value.y);
}

float value_noise(vec2 value)
{
    vec2 cell = floor(value);
    vec2 local = fract(value);
    local = local * local * (3.0 - 2.0 * local);
    float a = hash21(cell);
    float b = hash21(cell + vec2(1.0, 0.0));
    float c = hash21(cell + vec2(0.0, 1.0));
    float d = hash21(cell + vec2(1.0, 1.0));
    return mix(mix(a, b, local.x), mix(c, d, local.x), local.y);
}

float fbm(vec2 value)
{
    float result = 0.0;
    float weight = 0.52;
    mat2 rotation = mat2(0.80, 0.60, -0.60, 0.80);
    for (int octave = 0; octave < 4; ++octave) {
        result += value_noise(value) * weight;
        value = rotation * value * 2.03 + vec2(7.1, 3.7);
        weight *= 0.48;
    }
    return result;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / max(resolution, vec2(1.0));
    vec2 point = uv - 0.5;
    point.x *= resolution.x / max(resolution.y, 1.0);

    float energy = clamp(rms_level * 0.55 + peak_level * 0.20 + bass_level * 0.25, 0.0, 1.0);
    float motion = 0.16 + activity * (0.52 + energy * 0.38);
    vec2 warped = point;
    warped += vec2(
        sin(point.y * 3.1 + visual_time * 0.72),
        sin(point.x * 2.7 - visual_time * 0.58))
        * (0.018 + mid_level * 0.055) * motion;

    float primary = fbm(warped * (2.05 + treble_level * 0.16) + vec2(visual_time * 0.055, -visual_time * 0.041));
    float secondary = fbm(warped * 4.15 + vec2(-visual_time * 0.034, visual_time * 0.046) + primary * 0.72);
    float radius = length(warped + vec2(primary - 0.5, secondary - 0.5) * 0.055);
    float field = primary * 0.68 + secondary * 0.25 + radius * (0.96 + bass_level * 0.30);
    field += sin(radius * 19.0 - visual_time * 2.4) * bass_level * 0.035;

    float contour_count = 7.0 + bass_level * 2.5 + treble_level * 1.5;
    float contour_phase = fract(field * contour_count);
    float contour_distance = min(contour_phase, 1.0 - contour_phase);
    float antialiasing = max(fwidth(field * contour_count), 0.004);
    float contour = 1.0 - smoothstep(
        0.025 + bass_level * 0.010,
        0.025 + bass_level * 0.010 + antialiasing * 1.35,
        contour_distance);

    float ring_radius = 0.10 + beat_age * 0.68;
    float ring_distance = abs(length(point) - ring_radius);
    float ripple = 1.0 - smoothstep(0.006, 0.024, ring_distance);
    ripple *= beat * (1.0 - smoothstep(0.15, 1.1, ring_radius));

    float edge_distance = length(point * vec2(0.78, 1.0));
    float readability = mix(0.58, 1.0, smoothstep(0.18, 0.82, edge_distance));
    float vignette = 1.0 - smoothstep(0.48, 1.08, edge_distance);
    float base = 0.024 + primary * 0.012 + secondary * 0.005;
    float line_strength = 0.017 + activity * 0.038 + energy * 0.075;
    float luminance = base + contour * line_strength * readability;
    luminance += ripple * (0.035 + peak_level * 0.105);

    float grain = hash21(gl_FragCoord.xy + floor(visual_time * 18.0)) - 0.5;
    luminance += grain * treble_level * 0.010;
    luminance *= 0.76 + vignette * 0.24;
    fragment_color = vec4(vec3(clamp(luminance, 0.0, 0.24)), 1.0);
}
)glsl";

#undef SOUNDSTEP_GLSL_PRECISION
#undef SOUNDSTEP_GLSL_VERSION

    GLuint _compile_shader(GLenum type, const char* source)
    {
        const GLuint _shader = glCreateShader(type);
        if (_shader == 0) {
            throw renderer_error("Could not create the background shader");
        }
        glShaderSource(_shader, 1, &source, nullptr);
        glCompileShader(_shader);

        GLint _compiled = GL_FALSE;
        glGetShaderiv(_shader, GL_COMPILE_STATUS, &_compiled);
        if (_compiled == GL_TRUE) {
            return _shader;
        }

        GLint _log_size = 0;
        glGetShaderiv(_shader, GL_INFO_LOG_LENGTH, &_log_size);
        std::vector<char> _log(static_cast<std::size_t>((std::max)(_log_size, 1)));
        glGetShaderInfoLog(_shader, _log_size, nullptr, _log.data());
        const std::string _message = "Could not compile the background shader: " + std::string(_log.data());
        glDeleteShader(_shader);
        throw renderer_error(_message);
    }

    GLuint _create_background_program()
    {
        const GLuint _vertex = _compile_shader(GL_VERTEX_SHADER, _background_vertex_shader);
        GLuint _fragment = 0;
        GLuint _program = 0;
        try {
            _fragment = _compile_shader(GL_FRAGMENT_SHADER, _background_fragment_shader);
            _program = glCreateProgram();
            if (_program == 0) {
                throw renderer_error("Could not create the background shader program");
            }
            glAttachShader(_program, _vertex);
            glAttachShader(_program, _fragment);
            glLinkProgram(_program);

            GLint _linked = GL_FALSE;
            glGetProgramiv(_program, GL_LINK_STATUS, &_linked);
            if (_linked != GL_TRUE) {
                GLint _log_size = 0;
                glGetProgramiv(_program, GL_INFO_LOG_LENGTH, &_log_size);
                std::vector<char> _log(static_cast<std::size_t>((std::max)(_log_size, 1)));
                glGetProgramInfoLog(_program, _log_size, nullptr, _log.data());
                throw renderer_error("Could not link the background shader: " + std::string(_log.data()));
            }
        } catch (...) {
            if (_program != 0) {
                glDeleteProgram(_program);
            }
            if (_fragment != 0) {
                glDeleteShader(_fragment);
            }
            glDeleteShader(_vertex);
            throw;
        }

        glDetachShader(_program, _vertex);
        glDetachShader(_program, _fragment);
        glDeleteShader(_vertex);
        glDeleteShader(_fragment);
        return _program;
    }

    float _smoothed_level(float current, float target, float attack, float release, float delta_time)
    {
        const float _rate = target > current ? attack : release;
        return current + (target - current) * (1.0f - std::exp(-_rate * delta_time));
    }

    void _apply_dark_grey_palette()
    {
        ImVec4* _colors = ImGui::GetStyle().Colors;
        _colors[ImGuiCol_Border] = ImVec4(0.30f, 0.30f, 0.30f, 0.65f);
        _colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.16f, 0.75f);
        _colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
        _colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
        _colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
        _colors[ImGuiCol_CheckMark] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
        _colors[ImGuiCol_SliderGrab] = ImVec4(0.42f, 0.42f, 0.42f, 1.00f);
        _colors[ImGuiCol_SliderGrabActive] = ImVec4(0.58f, 0.58f, 0.58f, 1.00f);
        _colors[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
        _colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
        _colors[ImGuiCol_ButtonActive] = ImVec4(0.34f, 0.34f, 0.34f, 1.00f);
        _colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.20f, 0.75f);
        _colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
        _colors[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
        _colors[ImGuiCol_SeparatorHovered] = ImVec4(0.38f, 0.38f, 0.38f, 0.78f);
        _colors[ImGuiCol_SeparatorActive] = ImVec4(0.48f, 0.48f, 0.48f, 1.00f);
        _colors[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.30f, 0.30f, 0.20f);
        _colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.42f, 0.42f, 0.42f, 0.67f);
        _colors[ImGuiCol_ResizeGripActive] = ImVec4(0.55f, 0.55f, 0.55f, 0.95f);
        _colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
        _colors[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
        _colors[ImGuiCol_TabActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
        _colors[ImGuiCol_TabUnfocused] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
        _colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
        _colors[ImGuiCol_TableHeaderBg] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
        _colors[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
        _colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
        _colors[ImGuiCol_TextSelectedBg] = ImVec4(0.45f, 0.45f, 0.45f, 0.35f);
        _colors[ImGuiCol_NavHighlight] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
        _colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.86f);
    }

}

namespace {

    struct background_program {
        background_program()
            : _program(_create_background_program())
        {
            glGenVertexArrays(1, &_vertex_array);
            if (_vertex_array == 0) {
                glDeleteProgram(_program);
                _program = 0;
                throw renderer_error("Could not create the background shader geometry");
            }

            _resolution_location = glGetUniformLocation(_program, "resolution");
            _time_location = glGetUniformLocation(_program, "visual_time");
            _activity_location = glGetUniformLocation(_program, "activity");
            _rms_location = glGetUniformLocation(_program, "rms_level");
            _peak_location = glGetUniformLocation(_program, "peak_level");
            _bass_location = glGetUniformLocation(_program, "bass_level");
            _mid_location = glGetUniformLocation(_program, "mid_level");
            _treble_location = glGetUniformLocation(_program, "treble_level");
            _beat_location = glGetUniformLocation(_program, "beat");
            _beat_age_location = glGetUniformLocation(_program, "beat_age");
        }

        ~background_program()
        {
            if (_vertex_array != 0) {
                glDeleteVertexArrays(1, &_vertex_array);
            }
            if (_program != 0) {
                glDeleteProgram(_program);
            }
        }

        void draw(int width, int height, const playback_status& playback, float delta_time)
        {
            if (width <= 0 || height <= 0) {
                return;
            }

            const float _dt = (std::clamp)(delta_time, 0.0f, 0.05f);
            const float _activity_target = playback.state == playback_state::playing
                ? 1.0f
                : (playback.state == playback_state::buffering ? 0.24f : 0.0f);
            const float _rms_target = (std::clamp)(playback.rms_level * 3.6f, 0.0f, 1.0f);
            const float _peak_target = (std::clamp)(playback.peak_level * 1.35f, 0.0f, 1.0f);
            const float _bass_target = (std::clamp)(playback.bass_level * 5.0f, 0.0f, 1.0f);
            const float _mid_target = (std::clamp)(playback.mid_level * 4.2f, 0.0f, 1.0f);
            const float _treble_target = (std::clamp)(playback.treble_level * 5.5f, 0.0f, 1.0f);

            _activity = _smoothed_level(_activity, _activity_target, 7.5f, 2.0f, _dt);
            _rms = _smoothed_level(_rms, _rms_target, 12.0f, 3.0f, _dt);
            _peak = _smoothed_level(_peak, _peak_target, 18.0f, 4.5f, _dt);
            _bass = _smoothed_level(_bass, _bass_target, 11.0f, 2.6f, _dt);
            _mid = _smoothed_level(_mid, _mid_target, 10.0f, 3.2f, _dt);
            _treble = _smoothed_level(_treble, _treble_target, 15.0f, 5.0f, _dt);

            _beat_cooldown = (std::max)(0.0f, _beat_cooldown - _dt);
            if (_activity_target > 0.9f && _bass_target - _previous_bass_target > 0.075f && _peak_target > 0.20f && _beat_cooldown <= 0.0f) {
                _beat = 1.0f;
                _beat_age = 0.0f;
                _beat_cooldown = 0.16f;
            }
            _previous_bass_target = _bass_target;
            _beat *= std::exp(-5.2f * _dt);
            _beat_age += _dt;
            _visual_time += _dt * (0.08f + _activity * (0.46f + _mid * 0.24f));

            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glUseProgram(_program);
            glUniform2f(_resolution_location, static_cast<float>(width), static_cast<float>(height));
            glUniform1f(_time_location, _visual_time);
            glUniform1f(_activity_location, _activity);
            glUniform1f(_rms_location, _rms);
            glUniform1f(_peak_location, _peak);
            glUniform1f(_bass_location, _bass);
            glUniform1f(_mid_location, _mid);
            glUniform1f(_treble_location, _treble);
            glUniform1f(_beat_location, _beat);
            glUniform1f(_beat_age_location, _beat_age);
            glBindVertexArray(_vertex_array);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glUseProgram(0);
        }

        GLuint _program { 0 };
        GLuint _vertex_array { 0 };
        GLint _resolution_location { -1 };
        GLint _time_location { -1 };
        GLint _activity_location { -1 };
        GLint _rms_location { -1 };
        GLint _peak_location { -1 };
        GLint _bass_location { -1 };
        GLint _mid_location { -1 };
        GLint _treble_location { -1 };
        GLint _beat_location { -1 };
        GLint _beat_age_location { -1 };
        float _visual_time { 0.0f };
        float _activity { 0.0f };
        float _rms { 0.0f };
        float _peak { 0.0f };
        float _bass { 0.0f };
        float _mid { 0.0f };
        float _treble { 0.0f };
        float _previous_bass_target { 0.0f };
        float _beat { 0.0f };
        float _beat_age { 10.0f };
        float _beat_cooldown { 0.0f };
    };

}

struct renderer::implementation {
#ifdef __ANDROID__
    explicit implementation(ANativeWindow* native_window)
        : window(native_window)
    {
    }

    ANativeWindow* window { nullptr };
#else
    explicit implementation(std::shared_ptr<GLFWwindow> native_window)
        : window(std::move(native_window))
    {
    }

    std::shared_ptr<GLFWwindow> window { nullptr };
#endif
    std::unique_ptr<background_program> background { nullptr };
};

#ifdef __ANDROID__
renderer::renderer(ANativeWindow* window)
#else
renderer::renderer(std::shared_ptr<GLFWwindow> window)
#endif
    : _implementation(std::make_unique<implementation>(std::move(window)))
{
#ifndef __ANDROID__
    if (gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)) == 0) {
        throw renderer_error("Failed to initialize the OpenGL function loader");
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    _apply_dark_grey_palette();
    ImGuiStyle& _style = ImGui::GetStyle();
    _style.FramePadding = ImVec2(5.0f, 5.0f);
    constexpr float _rounding = 6.0f;
    _style.WindowRounding = _rounding;
    _style.ChildRounding = _rounding;
    _style.PopupRounding = _rounding;
    _style.FrameRounding = _rounding;
    _style.ScrollbarRounding = _rounding;
    _style.GrabRounding = _rounding;
    _style.TabRounding = _rounding;

#ifdef __ANDROID__
    if (!ImGui_ImplAndroid_Init(_implementation->window)) {
        ImGui::DestroyContext();
        throw renderer_error("Failed to initialize the ImGui Android backend");
    }
#else
    if (!ImGui_ImplGlfw_InitForOpenGL(_implementation->window.get(), true)) {
        ImGui::DestroyContext();
        throw renderer_error("Failed to initialize the ImGui GLFW backend");
    }
#endif

    if (!ImGui_ImplOpenGL3_Init(
#ifdef __ANDROID__
            "#version 300 es"
#else
            "#version 330"
#endif
            )) {
#ifdef __ANDROID__
        ImGui_ImplAndroid_Shutdown();
#else
        ImGui_ImplGlfw_Shutdown();
#endif
        ImGui::DestroyContext();
        throw renderer_error("Failed to initialize the ImGui OpenGL backend");
    }

    try {
        _implementation->background = std::make_unique<background_program>();
    } catch (...) {
        ImGui_ImplOpenGL3_Shutdown();
#ifdef __ANDROID__
        ImGui_ImplAndroid_Shutdown();
#else
        ImGui_ImplGlfw_Shutdown();
#endif
        ImGui::DestroyContext();
        throw;
    }
}

renderer::~renderer()
{
    _implementation->background.reset();
    ImGui_ImplOpenGL3_Shutdown();
#ifdef __ANDROID__
    ImGui_ImplAndroid_Shutdown();
#else
    ImGui_ImplGlfw_Shutdown();
#endif
    ImGui::DestroyContext();
}

ImFont* renderer::add_font(std::string_view resource_path, float font_size)
{
    const std::string _path(resource_path);
    const cmrc::embedded_filesystem _resources = cmrc::soundstep_resource::get_filesystem();
    if (!_resources.is_file(_path)) {
        throw renderer_error("Embedded font not found: " + _path);
    }

    const cmrc::file _data = _resources.open(_path);
    const std::size_t _data_size = static_cast<std::size_t>(_data.end() - _data.begin());
    if (_data_size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw renderer_error("Embedded font is too large: " + _path);
    }

    ImGuiIO& _io = ImGui::GetIO();
    ImFontConfig _config;
    _config.FontDataOwnedByAtlas = false;
    ImFont* _font = _io.Fonts->AddFontFromMemoryTTF(const_cast<char*>(_data.begin()), static_cast<int>(_data_size), font_size, &_config);
    if (_font == nullptr) {
        throw renderer_error("Failed to load embedded font: " + _path);
    }
    return _font;
}

void renderer::merge_font(std::string_view resource_path, float font_size, ImFont* destination, const ImWchar* glyph_ranges)
{
    if (destination == nullptr) {
        throw renderer_error("Cannot merge an embedded font into a null destination");
    }

    const std::string _path(resource_path);
    const cmrc::embedded_filesystem _resources = cmrc::soundstep_resource::get_filesystem();
    if (!_resources.is_file(_path)) {
        throw renderer_error("Embedded font not found: " + _path);
    }

    const cmrc::file _data = _resources.open(_path);
    const std::size_t _data_size = static_cast<std::size_t>(_data.end() - _data.begin());
    if (_data_size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw renderer_error("Embedded font is too large: " + _path);
    }

    ImFontConfig _config;
    _config.FontDataOwnedByAtlas = false;
    _config.MergeMode = true;
    _config.DstFont = destination;
    _config.PixelSnapH = true;
    _config.GlyphOffset.y = 3.0f;
    ImFont* _font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<char*>(_data.begin()), static_cast<int>(_data_size), font_size, &_config, glyph_ranges);
    if (_font == nullptr) {
        throw renderer_error("Failed to merge embedded font: " + _path);
    }
}

void renderer::begin_frame()
{
    ImGui_ImplOpenGL3_NewFrame();
#ifdef __ANDROID__
    ImGui_ImplAndroid_NewFrame();
#else
    ImGui_ImplGlfw_NewFrame();
#endif
    ImGui::NewFrame();
    iam_update_begin_frame();
    iam_clip_update(ImGui::GetIO().DeltaTime);
}

void renderer::render(const playback_status& playback)
{
    ImGui::Render();

    int _width = 0;
    int _height = 0;
#ifdef __ANDROID__
    _width = ANativeWindow_getWidth(_implementation->window);
    _height = ANativeWindow_getHeight(_implementation->window);
#else
    glfwGetFramebufferSize(_implementation->window.get(), &_width, &_height);
#endif
    glViewport(0, 0, _width, _height);
    glClearColor(0.055f, 0.055f, 0.055f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    _implementation->background->draw(_width, _height, playback, ImGui::GetIO().DeltaTime);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

renderer_texture renderer::create_rgba_texture(const unsigned char* pixels, int width, int height)
{
    if (pixels == nullptr || width <= 0 || height <= 0) {
        throw renderer_error("Invalid texture image");
    }

    GLuint _texture = 0;
    glGenTextures(1, &_texture);
    if (_texture == 0) {
        throw renderer_error("Failed to create texture");
    }

    glBindTexture(GL_TEXTURE_2D, _texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    return { static_cast<std::uintptr_t>(_texture), width, height };
}

void renderer::destroy_texture(renderer_texture texture) noexcept
{
    const GLuint _texture = static_cast<GLuint>(texture.native_id);
    if (_texture != 0) {
        glDeleteTextures(1, &_texture);
    }
}

}
