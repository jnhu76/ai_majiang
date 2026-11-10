#include "aimj/runtime.hpp"
#include <sherpa-onnx/c-api/c-api.h>
#include <stdexcept>
#include <utility>
namespace aimj {
namespace {
class SherpaVitsBackend final : public ITtsBackend {
public:
    SherpaVitsBackend(const std::string& model,const std::string& tokens,
                      const std::string& lexicon,int threads,
                      const std::string& rule_fars) {
        if(model.empty() || tokens.empty()) throw std::invalid_argument("TTS model and tokens required");
        SherpaOnnxOfflineTtsConfig config{};
        config.model.vits.model=model.c_str();
        config.model.vits.tokens=tokens.c_str();
        config.model.vits.lexicon=lexicon.c_str();
        config.model.num_threads=threads;
        config.rule_fars=rule_fars.c_str();
        tts_=SherpaOnnxCreateOfflineTts(&config);
        if(!tts_) throw std::runtime_error("SherpaOnnxCreateOfflineTts failed");
    }
    ~SherpaVitsBackend() override { if(tts_) SherpaOnnxDestroyOfflineTts(tts_); }
    AudioBuffer synthesize(const std::string& text,int speaker) override {
        auto* generated=SherpaOnnxOfflineTtsGenerate(tts_,text.c_str(),speaker-1,1.0f);
        if(!generated) throw std::runtime_error("sherpa generate failed");
        AudioBuffer audio;
        audio.sample_rate=generated->sample_rate;
        audio.samples.assign(generated->samples,generated->samples+generated->n);
        SherpaOnnxDestroyOfflineTtsGeneratedAudio(generated);
        return audio;
    }
    std::string name() const override {return "sherpa-onnx-vits";}
private:
    const SherpaOnnxOfflineTts* tts_{};
};
}
std::unique_ptr<ITtsBackend> make_sherpa_vits_backend(
    const std::string& model,const std::string& tokens,const std::string& lexicon,int threads,
    const std::string& rule_fars) {
    return std::make_unique<SherpaVitsBackend>(model,tokens,lexicon,threads,rule_fars);
}
} // namespace aimj
