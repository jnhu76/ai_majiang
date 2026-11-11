#include "aimj/audio_player.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace aimj {
namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

#if defined(_WIN32)
struct PlaybackState {
    std::atomic<bool> done{false};
};
void CALLBACK wave_out_callback(HWAVEOUT, UINT message, DWORD_PTR user,
                                DWORD_PTR, DWORD_PTR) {
    if (message == WOM_DONE)
        reinterpret_cast<PlaybackState*>(user)->done.store(true);
}

class WaveOutPlayer final : public IAudioPlayer {
public:
    PlaybackTiming play(const AudioBuffer& audio, const std::string& source) override {
        (void)source;
        PlaybackTiming timing;
        if (audio.sample_rate <= 0 || audio.samples.empty()) {
            timing.status = "unavailable";
            timing.error = "empty audio buffer";
            return timing;
        }
        std::vector<std::int16_t> pcm;
        pcm.reserve(audio.samples.size());
        for (float value : audio.samples) {
            if (value > 1.0f) value = 1.0f;
            if (value < -1.0f) value = -1.0f;
            pcm.push_back(static_cast<std::int16_t>(value * 32767.0f));
        }
        timing.audio_duration_ms =
            1000.0 * static_cast<double>(pcm.size()) / audio.sample_rate;

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = static_cast<DWORD>(audio.sample_rate);
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        PlaybackState state;
        HWAVEOUT handle = nullptr;
        const auto start = Clock::now();
        const MMRESULT open = waveOutOpen(&handle, WAVE_MAPPER, &format,
                                          reinterpret_cast<DWORD_PTR>(&wave_out_callback),
                                          reinterpret_cast<DWORD_PTR>(&state),
                                          CALLBACK_FUNCTION);
        if (open != MMSYSERR_NOERROR) {
            timing.status = "unavailable";
            timing.error = "waveOutOpen failed: " + std::to_string(open);
            return timing;
        }
        WAVEHDR header{};
        header.lpData = reinterpret_cast<LPSTR>(pcm.data());
        header.dwBufferLength = static_cast<DWORD>(pcm.size() * sizeof(std::int16_t));
        if (waveOutPrepareHeader(handle, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            timing.status = "unavailable";
            timing.error = "waveOutPrepareHeader failed";
            waveOutClose(handle);
            return timing;
        }
        if (waveOutWrite(handle, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            timing.status = "unavailable";
            timing.error = "waveOutWrite failed";
            waveOutUnprepareHeader(handle, &header, sizeof(header));
            waveOutClose(handle);
            return timing;
        }
        timing.submitted = true;
        timing.pcm_to_submit_ms = ms_since(start);
        const auto submitted_at = Clock::now();
        const auto timeout = std::chrono::milliseconds(
            static_cast<long long>(timing.audio_duration_ms) + 2000);
        while (!state.done.load() && Clock::now() - submitted_at < timeout)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        timing.submit_to_callback_ms = ms_since(submitted_at);
        timing.status = state.done.load() ? "ok" : "callback_timeout";
        if (!state.done.load()) timing.error = "waveOut buffer callback did not arrive";
        waveOutReset(handle);
        waveOutUnprepareHeader(handle, &header, sizeof(header));
        waveOutClose(handle);
        return timing;
    }
};
#else
class StubPlayer final : public IAudioPlayer {
public:
    PlaybackTiming play(const AudioBuffer&, const std::string&) override {
        PlaybackTiming timing;
        timing.status = "unavailable";
        timing.error = "waveOut playback is Windows-only in this prototype";
        return timing;
    }
};
#endif
} // namespace

std::unique_ptr<IAudioPlayer> make_audio_player() {
#if defined(_WIN32)
    return std::make_unique<WaveOutPlayer>();
#else
    return std::make_unique<StubPlayer>();
#endif
}
} // namespace aimj
