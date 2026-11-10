#include "aimj/runtime.hpp"
#include <llm/llm.hpp>
#include <chrono>
#include <memory>
#include <sstream>
#include <stdexcept>
namespace aimj {
namespace {
// Records the moment the first generated character is written to the output
// stream, which yields a measured time-to-first-character (TTFC). MNN's text
// response API exposes no token-level timestamps (LlmContext::ttfa_us is only
// populated on the omni/multimodal path), so TTFC bounds the true first token.
// Zero-length writes and EOF are not output and must not start the clock.
class FirstCharStreamBuf final : public std::stringbuf {
public:
    explicit FirstCharStreamBuf(Clock::time_point start) : start_(start) {}
    std::chrono::duration<double, std::milli> first_char_ms() const { return first_ms_; }
    bool got_first() const { return got_first_; }
protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (n > 0) mark_first();
        return std::stringbuf::xsputn(s, n);
    }
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) mark_first();
        return std::stringbuf::overflow(ch);
    }
private:
    void mark_first() {
        if (!got_first_) {
            got_first_ = true;
            first_ms_ = std::chrono::duration<double, std::milli>(Clock::now() - start_);
        }
    }
    Clock::time_point start_;
    std::chrono::duration<double, std::milli> first_ms_{};
    bool got_first_{false};
};
class MnnBackend final : public ILlmBackend {
public:
    explicit MnnBackend(const std::string& path, int max_new_tokens,
                        const std::string& backend_type, int thread_num)
        : max_new_tokens_(max_new_tokens) {
        if(path.empty()) throw std::invalid_argument("--mnn-config is required");
        if(max_new_tokens_ <= 0) throw std::invalid_argument("max_new_tokens must be positive");
        model_.reset(MNN::Transformer::Llm::createLLM(path));
        if(!model_) throw std::runtime_error("MNN createLLM failed");
        if(!backend_type.empty() || thread_num > 0) {
            // Merge runtime overrides into the parsed config before load(); the
            // pinned model directory keeps its original config.json on disk.
            std::string overrides = "{";
            if(!backend_type.empty()) {
                if(backend_type != "cpu" && backend_type != "opencl")
                    throw std::invalid_argument("unsupported backend_type: " + backend_type);
                overrides += "\"backend_type\": \"" + backend_type + "\"";
            }
            if(thread_num > 0) {
                if(!overrides.empty() && overrides.size() > 1) overrides += ", ";
                overrides += "\"thread_num\": " + std::to_string(thread_num);
            }
            overrides += "}";
            if(!model_->set_config(overrides))
                throw std::runtime_error("MNN set_config failed: " + overrides);
        }
        if(!model_->load()) throw std::runtime_error("MNN model load failed: " + path);
    }
    std::string generate(const std::string& prompt,int) override {
        // Each dialogue probe call is an independent turn: clear the context so
        // hundreds of sequential calls cannot drift through hidden KV/history.
        model_->reset();
        auto start = Clock::now();
        FirstCharStreamBuf buf(start);
        std::ostream out(&buf);
        // MNN applies the model's chat template to this plain user content.
        model_->response(prompt, &out, nullptr, max_new_tokens_);
        const auto* ctx = model_->getContext();
        usage_ = LlmUsage{
            ctx->prompt_len, ctx->gen_seq_len,
            static_cast<double>(ctx->prefill_us) / 1000.0,
            static_cast<double>(ctx->decode_us) / 1000.0,
            buf.got_first() ? buf.first_char_ms().count() : 0.0};
        // A silently empty response must surface as an error, not as a line of dialogue.
        std::string text = buf.str();
        if(text.empty() || usage_.generated_tokens <= 0)
            throw std::runtime_error("MNN produced no output tokens");
        return text;
    }
    LlmUsage last_usage() const override { return usage_; }
    std::string name() const override { return "mnn"; }
private:
    int max_new_tokens_;
    LlmUsage usage_{};
    std::unique_ptr<MNN::Transformer::Llm> model_;
};
}
std::unique_ptr<ILlmBackend> make_mnn_backend(const std::string& config_path, int max_new_tokens,
                                              const std::string& backend_type, int thread_num) {
    return std::make_unique<MnnBackend>(config_path, max_new_tokens, backend_type, thread_num);
}
} // namespace aimj
