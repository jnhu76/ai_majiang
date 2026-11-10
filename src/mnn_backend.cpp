#include "aimj/runtime.hpp"
#include <llm/llm.hpp>
#include <memory>
#include <sstream>
#include <stdexcept>
namespace aimj {
namespace {
class MnnBackend final : public ILlmBackend {
public:
    explicit MnnBackend(const std::string& path) {
        if(path.empty()) throw std::invalid_argument("--mnn-config is required");
        model_.reset(MNN::Transformer::Llm::createLLM(path));
        if(!model_) throw std::runtime_error("MNN createLLM failed");
        model_->load();
    }
    std::string generate(const std::string& prompt,int) override {
        std::ostringstream result;
        model_->response(prompt,&result);
        return result.str();
    }
    std::string name() const override { return "mnn"; }
private:
    std::unique_ptr<MNN::Transformer::Llm> model_;
};
}
std::unique_ptr<ILlmBackend> make_mnn_backend(const std::string& config_path) {
    return std::make_unique<MnnBackend>(config_path);
}
} // namespace aimj
