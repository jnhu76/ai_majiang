#pragma once
#include "aimj/runtime.hpp"
#include <string>
namespace aimj {
// Portable temporary playback bridge; output path must be writable and lifecycle-managed.
void write_pcm16_wav(const std::string& path,const AudioBuffer& audio);
}
