#pragma once
#include "aimj/runtime.hpp"

#include <memory>
#include <string>

namespace aimj {
struct PlaybackTiming {
    bool submitted{};
    std::string status{"unavailable"}; // ok | callback_timeout | unavailable
    std::string error;
    double pcm_to_submit_ms{};
    double submit_to_callback_ms{};
    double audio_duration_ms{};
    // These two are only true when a real measurement method exists. waveOut
    // submission/completion is neither first audio output nor first audible.
    bool first_audio_output_measured{false};
    bool first_audible_measured{false};
};

class IAudioPlayer {
public:
    virtual ~IAudioPlayer() = default;
    virtual PlaybackTiming play(const AudioBuffer& audio, const std::string& source) = 0;
};

std::unique_ptr<IAudioPlayer> make_audio_player();
} // namespace aimj
