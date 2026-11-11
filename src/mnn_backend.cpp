#include "aimj/runtime.hpp"
#include <llm/llm.hpp>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#include <windows.h>
#endif
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
#if defined(_WIN32)
std::wstring loaded_module_path(const wchar_t* module) {
    HMODULE handle = GetModuleHandleW(module);
    if (!handle) return {};
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(handle, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) return {};
    return std::wstring(buffer, n);
}
std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}
// The OpenCL build of MNN links the Khronos loader and embeds OpenCL C sources,
// so both names must appear in the DLL image. A CPU-only DLL contains neither;
// this is the cheap, offline half of the "was OpenCL really compiled in?" proof.
bool file_contains(const std::wstring& path, const char* needle) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return data.find(needle) != std::string::npos;
}
using ClGetPlatformIDs = int (*)(unsigned int, void**, unsigned int*);
bool probe_icd(std::string& detail) {
    HMODULE ocl = LoadLibraryW(L"OpenCL.dll");
    if (!ocl) {
        detail = "OpenCL.dll not loadable";
        return false;
    }
    auto get_platforms = reinterpret_cast<ClGetPlatformIDs>(GetProcAddress(ocl, "clGetPlatformIDs"));
    if (!get_platforms) {
        detail = "clGetPlatformIDs missing from OpenCL.dll";
        FreeLibrary(ocl);
        return false;
    }
    unsigned int count = 0;
    const int rc = get_platforms(0, nullptr, &count);
    detail = "clGetPlatformIDs rc=" + std::to_string(rc) + " platforms=" + std::to_string(count);
    FreeLibrary(ocl);
    return rc == 0 && count > 0;
}
#endif
// Fail fast instead of letting MNN's CPU fallback silently answer an OpenCL
// request: a run labelled "opencl" must never be produced by CPU kernels.
void require_opencl_backend() {
#if defined(_WIN32)
    const auto path = loaded_module_path(L"MNN.dll");
    if (path.empty())
        throw std::runtime_error("backend_unavailable: MNN.dll is not loaded in this process");
    if (!file_contains(path, "clCreateCommandQueue") || !file_contains(path, "__kernel"))
        throw std::runtime_error("backend_unavailable: loaded MNN.dll has no OpenCL symbols (rebuild with -DMNN_OPENCL=ON): " + utf8(path));
    std::string detail;
    if (!probe_icd(detail))
        throw std::runtime_error("backend_unavailable: no usable OpenCL ICD (" + detail + ")");
#else
    throw std::runtime_error("backend_unavailable: OpenCL backend verification is only implemented on Windows");
#endif
}
class MnnBackend final : public ILlmBackend {
public:
    explicit MnnBackend(const std::string& path, int max_new_tokens,
                        const std::string& backend_type, int thread_num)
        : backend_type_(backend_type.empty() ? "config_default" : backend_type),
          max_new_tokens_(max_new_tokens) {
        if(path.empty()) throw std::invalid_argument("--mnn-config is required");
        if(max_new_tokens_ <= 0) throw std::invalid_argument("max_new_tokens must be positive");
        if(!backend_type.empty() && backend_type != "cpu" && backend_type != "opencl")
            throw std::invalid_argument("unsupported backend_type: " + backend_type);
        if(backend_type == "opencl") require_opencl_backend();
        model_.reset(MNN::Transformer::Llm::createLLM(path));
        if(!model_) throw std::runtime_error("MNN createLLM failed");
        if(!backend_type.empty() || thread_num > 0) {
            // Merge runtime overrides into the parsed config before load(); the
            // pinned model directory keeps its original config.json on disk.
            std::string overrides = "{";
            if(!backend_type.empty())
                overrides += "\"backend_type\": \"" + backend_type + "\"";
            if(thread_num > 0) {
                if(overrides.size() > 1) overrides += ", ";
                overrides += "\"thread_num\": " + std::to_string(thread_num);
            }
            overrides += "}";
            if(!model_->set_config(overrides))
                throw std::runtime_error("MNN set_config failed: " + overrides);
        }
        if(!model_->load()) throw std::runtime_error("MNN model load failed: " + path);
    }
    std::string generate(const std::string& prompt,int) override {
        // Never expose the previous turn's usage if this call fails before the
        // response completes: the runtime reads last_usage() inside its try block.
        usage_ = LlmUsage{};
        // Each dialogue probe call is an independent turn: clear the context so
        // hundreds of sequential calls cannot drift through hidden KV/history.
        model_->reset();
        auto start = Clock::now();
        FirstCharStreamBuf buf(start);
        std::ostream out(&buf);
        // MNN applies the model's chat template to this plain user content.
        model_->response(prompt, &out, nullptr, max_new_tokens_);
        const auto* ctx = model_->getContext();
        if(!ctx) throw std::runtime_error("MNN context unavailable after response");
        // MNN's status is the only honest source for "stopped at max_new_tokens"
        // versus "finished at EOS" versus "failed mid-generation". The pinned
        // text path keeps RUNNING when it hits max_new_tokens (llm.cpp:986 has
        // no later rewrite) and sets NORMAL_FINISHED only at EOS (llm.cpp:1720).
        const auto status = ctx->status;
        const int status_code = static_cast<int>(status);
        usage_ = LlmUsage{
            .prompt_tokens = ctx->prompt_len,
            .generated_tokens = ctx->gen_seq_len,
            .prefill_ms = static_cast<double>(ctx->prefill_us) / 1000.0,
            .decode_ms = static_cast<double>(ctx->decode_us) / 1000.0,
            .ttfc_ms = buf.got_first() ? buf.first_char_ms().count() : 0.0,
            .truncated = status == MNN::Transformer::LlmStatus::RUNNING,
            .status_code = status_code,
        };
        std::string text = buf.str();
        if(status != MNN::Transformer::LlmStatus::NORMAL_FINISHED &&
           status != MNN::Transformer::LlmStatus::RUNNING)
            throw std::runtime_error("MNN response failed: status=" + std::to_string(status_code) +
                                     " generated_tokens=" + std::to_string(usage_.generated_tokens));
        // A silently empty response must surface as an error, not as a line of dialogue.
        if(text.empty() || usage_.generated_tokens <= 0)
            throw std::runtime_error("MNN produced no output tokens");
        return text;
    }
    LlmUsage last_usage() const override { return usage_; }
    std::string name() const override { return backend_type_ == "opencl" ? "mnn-opencl" : "mnn"; }
private:
    std::string backend_type_;
    int max_new_tokens_;
    LlmUsage usage_{};
    std::unique_ptr<MNN::Transformer::Llm> model_;
};
}
OpenClDiagnostics opencl_diagnostics() {
    OpenClDiagnostics d;
#if defined(_WIN32)
    const auto path = loaded_module_path(L"MNN.dll");
    d.mnn_dll_loaded = !path.empty();
    d.mnn_dll_path = utf8(path);
    if (d.mnn_dll_loaded) {
        d.mnn_opencl_symbols = file_contains(path, "clCreateCommandQueue") &&
                               file_contains(path, "__kernel");
    }
    d.icd_available = probe_icd(d.icd_detail);
#else
    d.icd_detail = "OpenCL diagnostics are only implemented on Windows";
#endif
    return d;
}
std::unique_ptr<ILlmBackend> make_mnn_backend(const std::string& config_path, int max_new_tokens,
                                              const std::string& backend_type, int thread_num) {
    return std::make_unique<MnnBackend>(config_path, max_new_tokens, backend_type, thread_num);
}
} // namespace aimj
