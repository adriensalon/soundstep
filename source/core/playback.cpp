#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include <miniaudio.h>

#include <core/playback.hpp>

namespace soundstep {
namespace {

    constexpr ma_uint32 _output_sample_rate = 48000;
    constexpr ma_uint32 _output_channels = 2;
    constexpr ma_uint32 _buffer_frame_count = _output_sample_rate * 2;
    constexpr ma_uint32 _prebuffer_frame_count = _output_sample_rate / 4;
    constexpr ma_uint32 _decode_batch_frame_count = 4096;

    std::string _miniaudio_error(std::string message, ma_result result)
    {
        message += ": ";
        message += ma_result_description(result);
        return message;
    }

    ma_encoding_format _miniaudio_encoding(audio_extension extension)
    {
        switch (extension) {
        case audio_extension::mp3:
            return ma_encoding_format_mp3;
        case audio_extension::wav:
            return ma_encoding_format_wav;
        case audio_extension::flac:
            return ma_encoding_format_flac;
        case audio_extension::ogg:
            return ma_encoding_format_vorbis;
        case audio_extension::unknown:
            throw playback_error("Audio extension is not supported");
        }
        throw playback_error("Audio extension is not supported");
    }

}

struct playback::implementation {
    implementation()
    {
        ma_result _result = ma_pcm_rb_init(
            ma_format_f32,
            _output_channels,
            _buffer_frame_count,
            nullptr,
            nullptr,
            &_buffer);
        if (_result != MA_SUCCESS) {
            throw playback_error(_miniaudio_error("Could not create playback buffer", _result));
        }
        _buffer_initialized = true;

        ma_device_config _configuration = ma_device_config_init(ma_device_type_playback);
        _configuration.playback.format = ma_format_f32;
        _configuration.playback.channels = _output_channels;
        _configuration.sampleRate = _output_sample_rate;
        _configuration.dataCallback = device_callback;
        _configuration.pUserData = this;

        _result = ma_device_init(nullptr, &_configuration, &_device);
        if (_result != MA_SUCCESS) {
            ma_pcm_rb_uninit(&_buffer);
            _buffer_initialized = false;
            throw playback_error(_miniaudio_error("Could not initialize the playback device", _result));
        }
        _device_initialized = true;
    }

    ~implementation()
    {
        std::lock_guard<std::mutex> _lock(_control_mutex);
        _play_requested.store(false, std::memory_order_release);
        stop_device();
        stop_decoder_thread();
        release_decoder();

        if (_device_initialized) {
            ma_device_uninit(&_device);
        }
        if (_buffer_initialized) {
            ma_pcm_rb_uninit(&_buffer);
        }
    }

    static ma_result decoder_read(ma_decoder* decoder, void* destination, std::size_t size, std::size_t* bytes_read)
    {
        implementation* _self = static_cast<implementation*>(decoder->pUserData);
        if (_self == nullptr || _self->_source == nullptr || bytes_read == nullptr) {
            return MA_INVALID_ARGS;
        }

        try {
            *bytes_read = _self->_source->read(static_cast<std::byte*>(destination), size);
            return MA_SUCCESS;
        } catch (...) {
            *bytes_read = 0;
            return MA_ERROR;
        }
    }

    static ma_result decoder_seek(ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin)
    {
        implementation* _self = static_cast<implementation*>(decoder->pUserData);
        if (_self == nullptr || _self->_source == nullptr) {
            return MA_INVALID_ARGS;
        }

        try {
            std::uint64_t _base = 0;
            if (origin == ma_seek_origin_current) {
                _base = _self->_source->tell();
            } else if (origin == ma_seek_origin_end) {
                _base = _self->_source->size();
            }

            std::uint64_t _target = 0;
            if (offset < 0) {
                const std::uint64_t _magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
                if (_magnitude > _base) {
                    return MA_BAD_SEEK;
                }
                _target = _base - _magnitude;
            } else {
                const std::uint64_t _positive_offset = static_cast<std::uint64_t>(offset);
                if (_positive_offset > (std::numeric_limits<std::uint64_t>::max)() - _base) {
                    return MA_BAD_SEEK;
                }
                _target = _base + _positive_offset;
            }

            return _self->_source->seek(_target) ? MA_SUCCESS : MA_BAD_SEEK;
        } catch (...) {
            return MA_BAD_SEEK;
        }
    }

    static void device_callback(ma_device* device, void* output, const void*, ma_uint32 frame_count)
    {
        implementation* _self = static_cast<implementation*>(device->pUserData);
        if (_self != nullptr) {
            _self->read_output(static_cast<float*>(output), frame_count);
        }
    }

    void read_output(float* output, ma_uint32 frame_count) noexcept
    {
        if (!_play_requested.load(std::memory_order_acquire) || _state.load(std::memory_order_acquire) != playback_state::playing) {
            std::memset(output, 0, static_cast<std::size_t>(frame_count) * _output_channels * sizeof(float));
            clear_analysis_levels();
            return;
        }

        ma_uint32 _frames_written = 0;
        const float _current_volume = _volume.load(std::memory_order_relaxed);

        while (_frames_written < frame_count) {
            ma_uint32 _frames_to_read = frame_count - _frames_written;
            void* _source_frames = nullptr;
            if (ma_pcm_rb_acquire_read(&_buffer, &_frames_to_read, &_source_frames) != MA_SUCCESS || _frames_to_read == 0) {
                break;
            }

            const float* _input = static_cast<const float*>(_source_frames);
            float* _destination = output + static_cast<std::size_t>(_frames_written) * _output_channels;
            const std::size_t _sample_count = static_cast<std::size_t>(_frames_to_read) * _output_channels;
            for (std::size_t _index = 0; _index < _sample_count; ++_index) {
                _destination[_index] = _input[_index] * _current_volume;
            }

            ma_pcm_rb_commit_read(&_buffer, _frames_to_read);
            _frames_written += _frames_to_read;
        }

        _consumed_frames.fetch_add(_frames_written, std::memory_order_relaxed);

        if (_frames_written < frame_count) {
            float* _remaining = output + static_cast<std::size_t>(_frames_written) * _output_channels;
            std::memset(
                _remaining,
                0,
                static_cast<std::size_t>(frame_count - _frames_written) * _output_channels * sizeof(float));

            if (_decoder_finished.load(std::memory_order_acquire) && ma_pcm_rb_available_read(&_buffer) == 0) {
                _play_requested.store(false, std::memory_order_release);
                _state.store(playback_state::finished, std::memory_order_release);
            } else {
                _state.store(playback_state::buffering, std::memory_order_release);
            }
        }

        analyze_output(output, frame_count);
    }

    void analyze_output(const float* output, ma_uint32 frame_count) noexcept
    {
        if (frame_count == 0) {
            clear_analysis_levels();
            return;
        }

        // Two inexpensive one-pole low-pass filters split the audible output
        // into broad bass, mid, and treble energy without doing FFT work on
        // the real-time audio thread.
        constexpr float _bass_alpha = 0.02329f; // Approximately 180 Hz at 48 kHz.
        constexpr float _mid_alpha = 0.26960f; // Approximately 2.4 kHz at 48 kHz.
        double _sum_squares = 0.0;
        double _bass_squares = 0.0;
        double _mid_squares = 0.0;
        double _treble_squares = 0.0;
        float _peak = 0.0f;

        for (ma_uint32 _frame = 0; _frame < frame_count; ++_frame) {
            const std::size_t _offset = static_cast<std::size_t>(_frame) * _output_channels;
            const float _left = output[_offset];
            const float _right = output[_offset + 1];
            const float _mono = (_left + _right) * 0.5f;
            _bass_filter += _bass_alpha * (_mono - _bass_filter);
            _mid_filter += _mid_alpha * (_mono - _mid_filter);
            const float _bass = _bass_filter;
            const float _mid = _mid_filter - _bass_filter;
            const float _treble = _mono - _mid_filter;

            _sum_squares += static_cast<double>(_left) * _left
                + static_cast<double>(_right) * _right;
            _bass_squares += static_cast<double>(_bass) * _bass;
            _mid_squares += static_cast<double>(_mid) * _mid;
            _treble_squares += static_cast<double>(_treble) * _treble;
            _peak = (std::max)(_peak, (std::max)(std::abs(_left), std::abs(_right)));
        }

        const double _inverse_frames = 1.0 / static_cast<double>(frame_count);
        _rms_level.store(
            static_cast<float>(std::sqrt(_sum_squares * _inverse_frames * 0.5)),
            std::memory_order_relaxed);
        _peak_level.store(_peak, std::memory_order_relaxed);
        _bass_level.store(
            static_cast<float>(std::sqrt(_bass_squares * _inverse_frames)),
            std::memory_order_relaxed);
        _mid_level.store(
            static_cast<float>(std::sqrt(_mid_squares * _inverse_frames)),
            std::memory_order_relaxed);
        _treble_level.store(
            static_cast<float>(std::sqrt(_treble_squares * _inverse_frames)),
            std::memory_order_relaxed);
    }

    void clear_analysis_levels() noexcept
    {
        _rms_level.store(0.0f, std::memory_order_relaxed);
        _peak_level.store(0.0f, std::memory_order_relaxed);
        _bass_level.store(0.0f, std::memory_order_relaxed);
        _mid_level.store(0.0f, std::memory_order_relaxed);
        _treble_level.store(0.0f, std::memory_order_relaxed);
    }

    void decode()
    {
        while (!_stop_thread.load(std::memory_order_acquire)) {
            ma_uint32 _writable = ma_pcm_rb_available_write(&_buffer);
            if (_writable == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            ma_uint32 _requested = (std::min)(_writable, _decode_batch_frame_count);
            void* _destination = nullptr;
            const ma_result _acquire_result = ma_pcm_rb_acquire_write(&_buffer, &_requested, &_destination);
            if (_acquire_result != MA_SUCCESS) {
                _state.store(playback_state::failed, std::memory_order_release);
                return;
            }

            ma_uint64 _decoded = 0;
            const ma_result _decode_result = ma_decoder_read_pcm_frames(
                &_decoder,
                _destination,
                _requested,
                &_decoded);
            ma_pcm_rb_commit_write(&_buffer, static_cast<ma_uint32>(_decoded));

            if (_decode_result != MA_SUCCESS && _decode_result != MA_AT_END) {
                _state.store(playback_state::failed, std::memory_order_release);
                _play_requested.store(false, std::memory_order_release);
                return;
            }

            if (_decoded == 0) {
                _decoder_finished.store(true, std::memory_order_release);
                ma_uint64 _length = 0;
                if (ma_decoder_get_cursor_in_pcm_frames(&_decoder, &_length) == MA_SUCCESS) {
                    _duration_frames.store(_length, std::memory_order_relaxed);
                }
                const ma_uint32 _readable = ma_pcm_rb_available_read(&_buffer);
                if (_play_requested.load(std::memory_order_acquire)) {
                    _state.store(
                        _readable == 0 ? playback_state::finished : playback_state::playing,
                        std::memory_order_release);
                }
                return;
            }

            if (_play_requested.load(std::memory_order_acquire) && _state.load(std::memory_order_acquire) == playback_state::buffering && ma_pcm_rb_available_read(&_buffer) >= _prebuffer_frame_count) {
                _state.store(playback_state::playing, std::memory_order_release);
            }
        }
    }

    void open(std::shared_ptr<audio_source> new_source, audio_extension extension)
    {
        if (new_source == nullptr) {
            throw playback_error("Cannot open a null audio source");
        }

        std::lock_guard<std::mutex> _lock(_control_mutex);
        _play_requested.store(false, std::memory_order_release);
        stop_device();
        stop_decoder_thread();
        release_decoder();
        ma_pcm_rb_reset(&_buffer);

        _source = std::move(new_source);
        if (!_source->seek(0)) {
            _source.reset();
            _state.store(playback_state::failed, std::memory_order_release);
            throw playback_error("Could not seek to the beginning of the audio source");
        }

        ma_decoder_config _configuration = ma_decoder_config_init(ma_format_f32, _output_channels, _output_sample_rate);
        _configuration.encodingFormat = _miniaudio_encoding(extension);
        const ma_result _result = ma_decoder_init(decoder_read, decoder_seek, this, &_configuration, &_decoder);
        if (_result != MA_SUCCESS) {
            _source.reset();
            _state.store(playback_state::failed, std::memory_order_release);
            throw playback_error(_miniaudio_error("Could not decode the audio source", _result));
        }
        _decoder_initialized = true;

        // Finding the length of some formats decodes the entire stream. Avoid
        // doing that here so opening a future peer source remains progressive.
        _duration_frames.store(0, std::memory_order_relaxed);
        _consumed_frames.store(0, std::memory_order_relaxed);
        _bass_filter = 0.0f;
        _mid_filter = 0.0f;
        clear_analysis_levels();
        _decoder_finished.store(false, std::memory_order_release);
        _state.store(playback_state::stopped, std::memory_order_release);
        start_decoder_thread();
    }

    void play()
    {
        std::lock_guard<std::mutex> _lock(_control_mutex);
        require_decoder();

        if (_state.load(std::memory_order_acquire) == playback_state::finished) {
            stop_device();
            seek_locked(0, false);
        }

        _play_requested.store(true, std::memory_order_release);
        const ma_uint32 _readable = ma_pcm_rb_available_read(&_buffer);
        const bool _ready = _readable >= _prebuffer_frame_count || (_decoder_finished.load(std::memory_order_acquire) && _readable != 0);
        _state.store(_ready ? playback_state::playing : playback_state::buffering, std::memory_order_release);

        if (!_device_started) {
            const ma_result _result = ma_device_start(&_device);
            if (_result != MA_SUCCESS) {
                _play_requested.store(false, std::memory_order_release);
                _state.store(playback_state::failed, std::memory_order_release);
                throw playback_error(_miniaudio_error("Could not start playback", _result));
            }
            _device_started = true;
        }
    }

    void pause()
    {
        std::lock_guard<std::mutex> _lock(_control_mutex);
        if (!_decoder_initialized) {
            return;
        }

        _play_requested.store(false, std::memory_order_release);
        stop_device();
        if (_state.load(std::memory_order_acquire) != playback_state::finished) {
            _state.store(playback_state::paused, std::memory_order_release);
        }
    }

    void stop()
    {
        std::lock_guard<std::mutex> _lock(_control_mutex);
        if (!_decoder_initialized) {
            _state.store(playback_state::stopped, std::memory_order_release);
            return;
        }

        _play_requested.store(false, std::memory_order_release);
        stop_device();
        seek_locked(0, false);
        _state.store(playback_state::stopped, std::memory_order_release);
    }

    void seek(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0.0) {
            throw playback_error("Playback position must be a finite, non-negative number");
        }

        const long double _requested_frames = static_cast<long double>(seconds) * _output_sample_rate;
        if (_requested_frames > static_cast<long double>((std::numeric_limits<ma_uint64>::max)())) {
            throw playback_error("Playback position is too large");
        }

        std::lock_guard<std::mutex> _lock(_control_mutex);
        require_decoder();

        const playback_state _previous_state = _state.load(std::memory_order_acquire);

        ma_uint64 _target = static_cast<ma_uint64>(_requested_frames);
        const ma_uint64 _length = _duration_frames.load(std::memory_order_relaxed);
        if (_length != 0) {
            _target = (std::min)(_target, _length);
        }

        const bool _resume = _play_requested.exchange(false, std::memory_order_acq_rel);
        stop_device();
        seek_locked(_target, _resume);
        if (!_resume) {
            _state.store(_previous_state == playback_state::stopped ? playback_state::stopped : playback_state::paused, std::memory_order_release);
        }
    }

    void seek_locked(ma_uint64 target, bool resume)
    {
        stop_decoder_thread();
        ma_pcm_rb_reset(&_buffer);

        const ma_result _result = ma_decoder_seek_to_pcm_frame(&_decoder, target);
        if (_result != MA_SUCCESS) {
            _state.store(playback_state::failed, std::memory_order_release);
            throw playback_error(_miniaudio_error("Could not seek the audio source", _result));
        }

        _consumed_frames.store(target, std::memory_order_relaxed);
        _decoder_finished.store(false, std::memory_order_release);
        start_decoder_thread();

        if (resume) {
            _play_requested.store(true, std::memory_order_release);
            _state.store(playback_state::buffering, std::memory_order_release);
            const ma_result _start_result = ma_device_start(&_device);
            if (_start_result != MA_SUCCESS) {
                _play_requested.store(false, std::memory_order_release);
                _state.store(playback_state::failed, std::memory_order_release);
                throw playback_error(_miniaudio_error("Could not resume playback", _start_result));
            }
            _device_started = true;
        }
    }

    playback_status status() const
    {
        std::lock_guard<std::mutex> _lock(_control_mutex);
        playback_status _result;
        _result.state = _state.load(std::memory_order_acquire);
        _result.has_source = _decoder_initialized;
        _result.position_seconds = static_cast<double>(_consumed_frames.load(std::memory_order_relaxed)) / _output_sample_rate;
        _result.duration_seconds = static_cast<double>(_duration_frames.load(std::memory_order_relaxed)) / _output_sample_rate;
        _result.buffered_seconds = static_cast<double>(ma_pcm_rb_available_read(const_cast<ma_pcm_rb*>(&_buffer))) / _output_sample_rate;
        if (_result.state == playback_state::playing) {
            _result.rms_level = _rms_level.load(std::memory_order_relaxed);
            _result.peak_level = _peak_level.load(std::memory_order_relaxed);
            _result.bass_level = _bass_level.load(std::memory_order_relaxed);
            _result.mid_level = _mid_level.load(std::memory_order_relaxed);
            _result.treble_level = _treble_level.load(std::memory_order_relaxed);
        }
        return _result;
    }

    void require_decoder() const
    {
        if (!_decoder_initialized) {
            throw playback_error("No audio source is open");
        }
    }

    void start_decoder_thread()
    {
        _stop_thread.store(false, std::memory_order_release);
        _decoder_thread = std::thread([this] { decode(); });
    }

    void stop_decoder_thread()
    {
        _stop_thread.store(true, std::memory_order_release);
        if (_decoder_thread.joinable()) {
            _decoder_thread.join();
        }
    }

    void stop_device()
    {
        if (_device_started) {
            ma_device_stop(&_device);
            _device_started = false;
        }
    }

    void release_decoder()
    {
        if (_decoder_initialized) {
            ma_decoder_uninit(&_decoder);
            _decoder_initialized = false;
        }
        _source.reset();
    }

    mutable std::mutex _control_mutex;
    std::shared_ptr<audio_source> _source;
    std::thread _decoder_thread;

    ma_decoder _decoder { };
    ma_device _device { };
    ma_pcm_rb _buffer { };

    bool _decoder_initialized { false };
    bool _device_initialized { false };
    bool _device_started { false };
    bool _buffer_initialized { false };

    std::atomic<bool> _stop_thread { false };
    std::atomic<bool> _play_requested { false };
    std::atomic<bool> _decoder_finished { false };
    std::atomic<float> _volume { 1.0f };
    std::atomic<float> _rms_level { 0.0f };
    std::atomic<float> _peak_level { 0.0f };
    std::atomic<float> _bass_level { 0.0f };
    std::atomic<float> _mid_level { 0.0f };
    std::atomic<float> _treble_level { 0.0f };
    std::atomic<ma_uint64> _consumed_frames { 0 };
    std::atomic<ma_uint64> _duration_frames { 0 };
    std::atomic<playback_state> _state { playback_state::stopped };
    float _bass_filter { 0.0f };
    float _mid_filter { 0.0f };
};

playback::playback()
    : _implementation(std::make_unique<implementation>())
{
}

playback::playback(playback&& other) noexcept = default;
playback& playback::operator=(playback&& other) noexcept = default;
playback::~playback() = default;

void playback::open(std::shared_ptr<audio_source> source, audio_extension extension)
{
    _implementation->open(std::move(source), extension);
}

void playback::open(const std::filesystem::path& path)
{
    const std::optional<audio_extension> _extension = audio_extension_from_path(path);
    if (!_extension) {
        throw playback_error("Audio file extension is missing or unsupported");
    }
    open(std::make_shared<file_audio_source>(path), *_extension);
}

void playback::play()
{
    _implementation->play();
}

void playback::pause()
{
    _implementation->pause();
}

void playback::stop()
{
    _implementation->stop();
}

void playback::seek(double seconds)
{
    _implementation->seek(seconds);
}

void playback::set_volume(float volume)
{
    if (!std::isfinite(volume)) {
        throw playback_error("Playback volume must be finite");
    }
    _implementation->_volume.store((std::clamp)(volume, 0.0f, 1.0f), std::memory_order_relaxed);
}

playback_status playback::status() const
{
    return _implementation->status();
}

}
