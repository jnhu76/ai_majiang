#include "aimj/runtime.hpp"
#include "aimj/wav.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace aimj;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
struct Case {
    std::string id, category, prompt, expected, forbidden;
    int speaker{};
};
struct Opt {
    std::string cases = "benchmarks/p1_dialogue_cases.tsv";
    std::string output = "benchmarks/results/p1-dialogue";
    std::string run_id = "unnamed";
    std::string llm = "mock", tts = "mock";
    std::string mnn_config, tts_model, tts_tokens, tts_lexicon, tts_rule_fars;
    int deadline_ms = 4000, max_tokens = 64, tts_threads = 2;
};
std::string json(const std::string& s) {
    constexpr char hex[] = "0123456789abcdef";
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                o += "\\u00";
                o += hex[c >> 4];
                o += hex[c & 15];
            } else {
                o += static_cast<char>(c);
            }
        }
    }
    return o + '"';
}
Opt parse(int argc, char** argv) {
    Opt o;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--help") {
            std::cout << "aimj_dialogue_probe --cases FILE --output DIR --run-id ID "
                "--llm mock|mnn --tts mock|sherpa --mnn-config PATH --max-tokens N "
                "--tts-model PATH --tts-tokens PATH --tts-lexicon PATH "
                "--tts-rule-fars PATH --tts-threads N --deadline-ms N\n";
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + k);
        std::string v = argv[++i];
        if (k == "--cases") o.cases = v;
        else if (k == "--output") o.output = v;
        else if (k == "--run-id") o.run_id = v;
        else if (k == "--llm") o.llm = v;
        else if (k == "--tts") o.tts = v;
        else if (k == "--mnn-config") o.mnn_config = v;
        else if (k == "--max-tokens") o.max_tokens = std::stoi(v);
        else if (k == "--tts-model") o.tts_model = v;
        else if (k == "--tts-tokens") o.tts_tokens = v;
        else if (k == "--tts-lexicon") o.tts_lexicon = v;
        else if (k == "--tts-rule-fars") o.tts_rule_fars = v;
        else if (k == "--tts-threads") o.tts_threads = std::stoi(v);
        else if (k == "--deadline-ms") o.deadline_ms = std::stoi(v);
        else throw std::invalid_argument("unknown option " + k);
    }
    if (o.deadline_ms < 1 || o.max_tokens < 1 || o.tts_threads < 1)
        throw std::invalid_argument("invalid positive numeric option");
    if (o.run_id.empty() || !std::all_of(o.run_id.begin(), o.run_id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        }))
        throw std::invalid_argument("run-id must contain ASCII letters, digits, - or _");
    return o;
}
std::vector<Case> load_cases(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open cases: " + path);
    std::vector<Case> cases;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::size_t begin = 0;
        for (std::size_t p = 0; p <= line.size(); ++p) {
            if (p == line.size() || line[p] == '\t') {
                fields.push_back(line.substr(begin, p - begin));
                begin = p + 1;
            }
        }
        if (fields.size() != 6)
            throw std::runtime_error("expected 6 TSV columns on line " + std::to_string(lineno));
        Case c{fields[0], fields[2], fields[3], fields[4], fields[5], std::stoi(fields[1])};
        if (c.speaker < 1 || c.speaker > 3 || c.prompt.empty() || c.id.empty() ||
            !std::all_of(c.id.begin(), c.id.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '_' || ch == '-';
            }))
            throw std::runtime_error("invalid ID/speaker/prompt on line " + std::to_string(lineno));
        for (const auto& previous : cases)
            if (previous.id == c.id) throw std::runtime_error("duplicate case id " + c.id);
        cases.push_back(std::move(c));
    }
    if (cases.empty()) throw std::runtime_error("no cases found");
    return cases;
}
double milliseconds(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}
} // namespace

int main(int argc, char** argv) {
    try {
        auto opt = parse(argc, argv);
        auto cases = load_cases(opt.cases);
        fs::path out = fs::path(opt.output) / opt.run_id;
        fs::create_directories(out);
        std::ofstream log(out / "results.jsonl", std::ios::binary | std::ios::trunc);
        if (!log) throw std::runtime_error("cannot open results.jsonl");
        std::unique_ptr<ILlmBackend> llm;
        std::unique_ptr<ITtsBackend> tts;
        auto init_start = Clock::now();
        if (opt.llm == "mock") llm = std::make_unique<MockLlm>(10ms);
#ifdef AIMJ_WITH_MNN
        else if (opt.llm == "mnn") llm = make_mnn_backend(opt.mnn_config, opt.max_tokens);
#endif
        else throw std::invalid_argument("LLM provider unavailable: " + opt.llm);
        if (opt.tts == "mock") tts = std::make_unique<MockTts>(10ms);
#ifdef AIMJ_WITH_SHERPA
        else if (opt.tts == "sherpa")
            tts = make_sherpa_vits_backend(opt.tts_model, opt.tts_tokens,
                                           opt.tts_lexicon, opt.tts_threads, opt.tts_rule_fars);
#endif
        else throw std::invalid_argument("TTS provider unavailable: " + opt.tts);
        double init_ms = milliseconds(Clock::now() - init_start);
        const bool real = opt.llm == "mnn" && opt.tts == "sherpa";
        const bool mixed = (opt.llm == "mock") != (opt.tts == "mock");
        log << "{\"type\":\"run\",\"schema\":1,\"run_id\":" << json(opt.run_id)
            << ",\"mode\":" << json(real ? "real" : mixed ? "mixed" : "mock")
            << ",\"llm_provider\":" << json(opt.llm)
            << ",\"tts_provider\":" << json(opt.tts)
            << ",\"cases\":" << cases.size()
            << ",\"max_new_tokens\":" << opt.max_tokens
            << ",\"deadline_ms\":" << opt.deadline_ms
            << ",\"provider_init_ms\":" << init_ms
            << ",\"first_audible_ms\":null}\n";
        AiRuntime runtime(std::move(llm), std::move(tts), 2);
        runtime.set_active_session(1);
        std::size_t ok = 0, failed = 0;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto& c = cases[i];
            DialogueRequest req{};
            req.session_id = 1;
            req.event_id = i + 1;
            req.speaker = c.speaker;
            req.persona = "";
            req.prompt = c.prompt;
            req.fallback_line = "（暂时无语音）";
            req.created = Clock::now();
            req.deadline = req.created + std::chrono::milliseconds(opt.deadline_ms);
            auto started = req.created;
            bool submitted = runtime.submit(std::move(req));
            std::optional<SpeechResult> r;
            if (submitted) {
                const auto until = started + std::chrono::milliseconds(opt.deadline_ms + 3000);
                while (Clock::now() < until) {
                    if ((r = runtime.poll())) break;
                    std::this_thread::sleep_for(1ms);
                }
            }
            std::string status = !submitted ? "queue_rejected" : !r ? "missing" : r->status;
            std::string wav;
            double audio_s = 0;
            double rtf = 0;
            if (r && status == "ok" && !r->audio.samples.empty() && r->audio.sample_rate > 0) {
                wav = c.id + ".wav";
                write_pcm16_wav((out / wav).string(), r->audio);
                audio_s = static_cast<double>(r->audio.samples.size()) / r->audio.sample_rate;
                rtf = (r->tts_ms / 1000.0) / audio_s;
                ++ok;
            } else {
                ++failed;
            }
            log << std::fixed << std::setprecision(3)
                << "{\"type\":\"case\",\"id\":" << json(c.id)
                << ",\"category\":" << json(c.category)
                << ",\"speaker\":" << c.speaker
                << ",\"expected_review_hint\":" << json(c.expected)
                << ",\"forbidden_review_hint\":" << json(c.forbidden)
                << ",\"status\":" << json(status)
                << ",\"generated_text\":" << json(r ? r->text : "")
                << ",\"audio_wav\":" << json(wav)
                << ",\"sample_rate\":" << (r ? r->audio.sample_rate : 0)
                << ",\"audio_s\":" << audio_s
                << ",\"queue_llm_ms\":" << (r ? r->queue_llm_ms : 0)
                << ",\"llm_ms\":" << (r ? r->llm_ms : 0)
                << ",\"llm_ttfc_ms\":" << (r ? r->llm_usage.ttfc_ms : 0)
                << ",\"prompt_tokens\":" << (r ? r->llm_usage.prompt_tokens : 0)
                << ",\"generated_tokens\":" << (r ? r->llm_usage.generated_tokens : 0)
                << ",\"tts_ms\":" << (r ? r->tts_ms : 0)
                << ",\"tts_rtf\":" << rtf
                << ",\"end_to_end_pcm_ms\":" << (r ? r->end_to_end_ms : 0)
                << ",\"first_audible_ms\":null}\n";
            std::cout << '[' << (i + 1) << '/' << cases.size() << "] " << c.id
                      << " " << status << " "
                      << (r ? r->text : "(no result)") << '\n';
        }
        log << "{\"type\":\"summary\",\"cases\":" << cases.size()
            << ",\"ok_with_wav\":" << ok << ",\"failed_or_missing\":" << failed
            << ",\"quality_human_graded\":false,\"first_audible_measured\":false}\n";
        log.flush();
        std::cout << "Results: " << (out / "results.jsonl").string()
                  << "; WAV=" << ok << "; failures=" << failed << '\n';
        return failed == 0 ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
