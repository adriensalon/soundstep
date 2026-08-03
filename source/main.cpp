#ifdef __ANDROID__
#include <android/log.h>
#include <android_native_app_glue.h>

#include <exception>
#include <filesystem>

#include <core/config.hpp>
#include <core/context.hpp>
#include <core/window.hpp>

extern "C" void android_main(android_app* app)
{
    app_dummy();
    try {
        const std::filesystem::path _data = std::filesystem::u8path(app->activity->internalDataPath);
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

        soundstep::context _ctx {
            _store, _scanner, _player, _server, _network, _offline, _covers
        };
        soundstep::window _window(_ctx, app);
        _window.run();
    } catch (const std::exception& exception) {
        __android_log_print(ANDROID_LOG_ERROR, "soundstep", "Soundstep failed: %s", exception.what());
        ANativeActivity_finish(app->activity);
    } catch (...) {
        __android_log_write(ANDROID_LOG_ERROR, "soundstep", "Soundstep failed with an unknown error");
        ANativeActivity_finish(app->activity);
    }
}

#else

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#include <core/config.hpp>
#include <core/context.hpp>
#include <core/window.hpp>

namespace {

std::filesystem::path _data_directory()
{
#ifdef _WIN32
    char* _local_app_data = nullptr;
    std::size_t _size = 0;
    if (_dupenv_s(&_local_app_data, &_size, "LOCALAPPDATA") == 0 && _local_app_data != nullptr) {
        const std::filesystem::path _result = std::filesystem::u8path(_local_app_data) / "Soundstep";
        std::free(_local_app_data);
        return _result;
    }
    std::free(_local_app_data);
#else
    if (const char* _local_app_data = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::u8path(_local_app_data) / "soundstep";
    }
#endif
    return std::filesystem::current_path() / ".soundstep";
}

}

namespace {

int run()
{
    try {
        const std::filesystem::path _data = _data_directory();
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
        soundstep::window _window(_ctx);
        _window.run();
        return 0;
    } catch (const std::exception& _exception) {
#ifdef _WIN32
        MessageBoxA(nullptr, _exception.what(), "Soundstep failed", MB_OK | MB_ICONERROR);
#else
        std::cerr << "Soundstep failed: " << _exception.what() << '\n';
#endif
        return 1;
    }
}

}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return run();
}
#else
int main()
{
    return run();
}
#endif
#endif
