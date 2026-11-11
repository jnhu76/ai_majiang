#pragma once
#include "aimj/runtime.hpp"
#include <cstdint>
#include <string>
namespace aimj {
// Playback bridge; output path must be writable and lifecycle-managed.
// Conversion is counted, not silent: the caller can tell whether the buffer
// was clipped or contained non-finite samples before it reaches a file.
struct WavWriteStats {
    std::uint64_t clipped_samples{};
    std::uint64_t non_finite_samples{};
};
WavWriteStats write_pcm16_wav(const std::string& path, const AudioBuffer& audio);
struct WavReadResult {
    AudioBuffer audio;
    // Samples already sitting at full scale in the file (+/-32767/-32768).
    // The writer's clipped_samples is the authoritative pre-truncation count.
    std::uint64_t clipped_samples{};
};
// Validates the RIFF/WAVE header and the PCM fmt block (mono 16-bit) instead of
// trusting the extension; throws std::runtime_error on a malformed file.
WavReadResult read_pcm16_wav(const std::string& path);
} // namespace aimj
