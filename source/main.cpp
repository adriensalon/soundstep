
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>

#if defined(__ANDROID__)
#include <android/log.h>
#include <android_native_app_glue.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

#include <core/config.hpp>
#include <core/context.hpp>
#include <core/window.hpp>

void log_message(const char* message)
{
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "soundstep", "%s", message);
#elif defined(_WIN32)
    MessageBoxA(nullptr, message, "soundstep", MB_OK | MB_ICONERROR);
#else
    std::cerr << "soundstep " << message << std::endl;
#endif
}

#if defined(__ANDROID__)
[[nodiscard]] std::filesystem::path _data_directory(android_app* app)
#else
[[nodiscard]] std::filesystem::path _data_directory()
#endif
{
#if defined(__ANDROID__)
    return std::filesystem::u8path(app->activity->internalDataPath);
#elif defined(_WIN32)
    char* _local_app_data = nullptr;
    std::size_t _size = 0;
    if (_dupenv_s(&_local_app_data, &_size, "LOCALAPPDATA") == 0 && _local_app_data != nullptr) {
        const std::filesystem::path _result = std::filesystem::u8path(_local_app_data) / "soundstep";
        std::free(_local_app_data);
        return _result;
    }
    std::free(_local_app_data);
#else
    if (const char* _local_app_data = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::u8path(_local_app_data) / "soundstep";
    }
#endif
    return { };
}

#if defined(__ANDROID__)
extern "C" void android_main(android_app* app)
#elif defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main()
#endif
{
#if defined(__ANDROID__)
    app_dummy();
#endif
    try {
#if defined(__ANDROID__)
        const std::filesystem::path _data = _data_directory(app);
#else
        const std::filesystem::path _data = _data_directory();
#endif
        soundstep::storage _store(_data / "soundstep.db", _data / "assets");
        const soundstep::configuration _configuration = _store.config();
        soundstep::library_scanner _scanner(_store);
        if (_configuration.scan_on_startup && !_configuration.library_path.empty()) {
            _scanner.scan();
        }
        soundstep::playback _player;
        soundstep::peer_server _server(_store);
        soundstep::peer_network _network(_store, _server);
        soundstep::offline_service _offline(_store);
        soundstep::cover_cache _covers(_store);

        soundstep::context _ctx { _store, _scanner, _player, _server, _network, _offline, _covers };
#if defined(__ANDROID__)
        soundstep::window _window(_ctx, app);
#else
        soundstep::window _window(_ctx);
#endif
        _window.run();
#if !defined(__ANDROID__)
        return 0;
#endif
    } catch (const std::exception& _exception) {
        log_message(_exception.what());
#if defined(__ANDROID__)
        ANativeActivity_finish(app->activity);
#else
        return 1;
#endif
    } catch (...) {
        log_message("An unknown error occurred");
#if defined(__ANDROID__)
        ANativeActivity_finish(app->activity);
#else
        return 1;
#endif
    }
}
