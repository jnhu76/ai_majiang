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
// Mock backends leave everything zeroed; provider identity in the JSONL run
// record is the authoritative REAL/MOCK classification, with usage as corroboration.
struct LlmUsage {
    int prompt_tokens{};
    int generated_tokens{};
    double prefill_ms{};
    double decode_ms{};
    // Time to first character (TTFC): generate() entry to the first output
    // character written to the stream. MNN's text API exposes no token-level
    // timestamps, so TTFC bounds (but is not identical to) time-to-first-token.
    double ttfc_ms{};
    // P1 audit fields, appended after the measured values so existing callers
    // keep their layout. Providers must initialize them with designated
    // initializers: a future field inserted mid-struct then fails to compile
    // instead of silently shifting values.
    bool truncated{};      // provider stopped at max_new_tokens, output is partial
    int status_code{-1};   // raw provider status enum; -1 = not reported
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
// backend_type ("cpu"/"opencl"/... ) and thread_num default to the values in the
// model config.json when empty/zero; overrides are merged via Llm::set_config
// before load(), so the pinned model directory itself is never modified.
// Requesting "opencl" fails with a "backend_unavailable:" error unless the
// loaded MNN.dll really carries OpenCL symbols and an ICD is present.
std::unique_ptr<ILlmBackend> make_mnn_backend(const std::string& config_path, int max_new_tokens,
                                              const std::string& backend_type = {}, int thread_num = 0);
// Evidence that an OpenCL request can be honoured, gathered from the process
// and the loaded binaries rather than from the request flags.
struct OpenClDiagnostics {
    bool mnn_dll_loaded{};
    std::string mnn_dll_path;
    bool mnn_opencl_symbols{};
    bool icd_available{};
    std::string icd_detail;
};
OpenClDiagnostics opencl_diagnostics();
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
    double end_to_end_ms{}; // created -> publish (PCM complete for status=="ok")
    LlmUsage llm_usage;
    // P1 audit trail, appended at the end. A fallback line replaces `text` when
    // a later stage fails, so the model's own line is preserved here instead of
    // being lost; `error_message` says why the stage failed.
    std::string raw_llm_text;
    std::string error_message;
    bool fallback_used{false};
    // When the result became available to the consumer (set at publish). The
    // probe subtracts this from its poll() timestamp to measure the handoff to
    // the playback layer without inventing a new runtime metric.
    Clock::time_point pcm_completed_at{};
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
