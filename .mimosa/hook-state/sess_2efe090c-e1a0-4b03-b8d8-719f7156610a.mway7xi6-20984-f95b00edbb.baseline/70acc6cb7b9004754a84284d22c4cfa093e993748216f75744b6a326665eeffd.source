#include "aimj/runtime.hpp"
#include <array>
#include <cmath>
#include <thread>
namespace aimj {
std::string MockLlm::generate(const std::string&, int speaker) {
    std::this_thread::sleep_for(delay_);
    constexpr std::array<const char*,3> lines{"嘿嘿，你的牌肥嘟嘟！", "啊？你又来这招！", "观察完毕，不予评价。"};
    return lines[static_cast<std::size_t>((speaker - 1 + 3) % 3)];
}
AudioBuffer MockTts::synthesize(const std::string&, int speaker) {
    std::this_thread::sleep_for(delay_);
    AudioBuffer audio;
    audio.samples.resize(1600); // 100ms synthetic PCM; NOT actual TTS
    for (std::size_t i=0;i<audio.samples.size();++i)
        audio.samples[i] = static_cast<float>(0.1 * std::sin(6.283185307179586 * (250 + speaker * 70) * static_cast<double>(i) / 16000));
    return audio;
}
} // namespace aimj
