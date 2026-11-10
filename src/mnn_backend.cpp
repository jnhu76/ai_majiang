#include "aimj/runtime.hpp"
#include <llm/llm.hpp>
#include <chrono>
#include <memory>
#include <sstream>
#include <stdexcept>
namespace aimj {
namespace {
// Records the moment the first generated character is written, which yields a
// measured time-to-first-token rather than a derived or fabricated value.
class FirstTokenStreamBuf final : public std::stringbuf {
public:
    explicit FirstTokenStreamBuf(Clock::time_point start) : start_(start) {}
    std::chrono::duration<double, std::milli> first_token_ms() const { return first_ms_; }
    bool got_first() const { return got_first_; }
protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        mark_first();
        return std::stringbuf::xsputn(s, n);
    }
    int_type overflow(int_type ch) override {
        mark_first();
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
    explicit MnnBackend(const std::string& path, int max_new_tokens)
        : max_new_tokens_(max_new_tokens) {
        if(path.empty()) throw std::invalid_argument("--mnn-config is required");
        if(max_new_tokens_ <= 0) throw std::invalid_argument("max_new_tokens must be positive");
        model_.reset(MNN::Transformer::Llm::createLLM(path));
        if(!model_) throw std::runtime_error("MNN createLLM failed");
        if(!model_->load()) throw std::runtime_error("MNN model load failed: " + path);
    }
    std::string generate(const std::string& prompt,int) override {
        auto start = Clock::now();
        FirstTokenStreamBuf buf(start);
        std::ostream out(&buf);
        // MNN applies the model's chat template to this plain user content.
        model_->response(prompt, &out, nullptr, max_new_tokens_);
        const auto* ctx = model_->getContext();
        usage_ = LlmUsage{
            ctx->prompt_len, ctx->gen_seq_len,
            static_cast<double>(ctx->prefill_us) / 1000.0,
            static_cast<double>(ctx->decode_us) / 1000.0,
            buf.got_first() ? buf.first_token_ms().count() : 0.0};
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
std::unique_ptr<ILlmBackend> make_mnn_backend(const std::string& config_path, int max_new_tokens) {
    return std::make_unique<MnnBackend>(config_path, max_new_tokens);
}
} // namespace aimj
