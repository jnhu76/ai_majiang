#pragma once
#include "aimj/director.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
namespace aimj {
struct AudioBuffer {
    int sample_rate{16000};
    std::vector<float> samples;
};
// Measured inference breakdown from the most recent generate() call.
// Mock backends leave everything zeroed, so JSONL records with non-zero
// token counts are provably from a real model.
struct LlmUsage {
    int prompt_tokens{};
    int generated_tokens{};
    double prefill_ms{};
    double decode_ms{};
    double ttft_ms{}; // generate() entry to first output character.
};
class ILlmBackend {
public:
    virtual ~ILlmBackend()=default;
    virtual std::string generate(const std::string& prompt, int speaker) = 0;
    virtual LlmUsage last_usage() const { return {}; }
    virtual std::string name() const = 0;
};
class ITtsBackend {
public:
    virtual ~ITtsBackend()=default;
    virtual AudioBuffer synthesize(const std::string& text, int speaker)=0;
    virtual std::string name() const = 0;
};
class MockLlm final : public ILlmBackend {
public:
    explicit MockLlm(std::chrono::milliseconds delay):delay_(delay){}
    std::string generate(const std::string& prompt,int speaker) override;
    std::string name() const override { return "mock-llm"; }
private:
    std::chrono::milliseconds delay_;
};
class MockTts final : public ITtsBackend {
public:
    explicit MockTts(std::chrono::milliseconds delay):delay_(delay){}
    AudioBuffer synthesize(const std::string& text,int speaker) override;
    std::string name() const override { return "mock-tts"; }
private:
    std::chrono::milliseconds delay_;
};
#ifdef AIMJ_WITH_MNN
std::unique_ptr<ILlmBackend> make_mnn_backend(const std::string& config_path, int max_new_tokens);
#endif
#ifdef AIMJ_WITH_SHERPA
std::unique_ptr<ITtsBackend> make_sherpa_vits_backend(
    const std::string& model, const std::string& tokens,
    const std::string& lexicon, int threads,
    const std::string& rule_fars = {});
#endif
struct SpeechResult {
    SessionId session_id{};
    EventId event_id{};
    int speaker{};
    std::string text;
    AudioBuffer audio;
    std::string status{"ok"}; // ok, timeout, stale, error
    double queue_llm_ms{};
    double llm_ms{};
    double queue_tts_ms{};
    double tts_ms{};
    double end_to_end_ms{};
    LlmUsage llm_usage;
};
struct RuntimeCounters {
    std::uint64_t accepted{};
    std::uint64_t queue_rejected{};
    std::uint64_t completed{};
    std::uint64_t expired{};
    std::uint64_t stale{};
    std::uint64_t failed{};
    std::uint64_t result_overflow{};
};
class AiRuntime final {
public:
    AiRuntime(std::unique_ptr<ILlmBackend> llm, std::unique_ptr<ITtsBackend> tts,
              std::size_t capacity=4);
    ~AiRuntime();
    AiRuntime(const AiRuntime&)=delete;
    AiRuntime& operator=(const AiRuntime&)=delete;
    bool submit(DialogueRequest req);
    std::optional<SpeechResult> poll();
    void set_active_session(SessionId id) { active_session_.store(id); }
    RuntimeCounters counters() const;
    std::size_t backlog() const;
    std::string llm_name() const { return llm_->name(); }
    std::string tts_name() const { return tts_->name(); }
private:
    struct TtsJob { DialogueRequest request; SpeechResult result; Clock::time_point queued; };
    void run_llm();
    void run_tts();
    void publish(SpeechResult result);
    std::unique_ptr<ILlmBackend> llm_;
    std::unique_ptr<ITtsBackend> tts_;
    std::size_t capacity_;
    std::atomic<SessionId> active_session_{1};
    mutable std::mutex llm_mu_,tts_mu_,results_mu_,counters_mu_;
    std::condition_variable_any llm_cv_,tts_cv_;
    std::deque<DialogueRequest> llm_queue_;
    std::deque<TtsJob> tts_queue_;
    std::deque<SpeechResult> results_;
    RuntimeCounters counts_{};
    std::atomic<bool> stopping_{false};
    std::thread llm_worker_,tts_worker_;
};
} // namespace aimj
