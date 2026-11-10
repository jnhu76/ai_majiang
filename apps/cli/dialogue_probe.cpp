#include "aimj/runtime.hpp"
#include "aimj/wav.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

using namespace aimj;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
struct Case {
    std::string id, category, prompt, expected, forbidden;
    int speaker{};
    // Optional columns (12-column TSV): machine-checkable keywords and the
    // structured material used to synthesize the P0/P2/P3 prompt strategies.
    std::string required_kw, forbidden_kw, persona_key, fact, history, structured;
};
struct TtsLine {
    std::string id, category, text;
    int speaker{};
};
struct Opt {
    std::string cases = "benchmarks/p1_dialogue_cases.tsv";
    std::string output = "benchmarks/results/p1-dialogue";
    std::string run_id = "unnamed";
    std::string llm = "mock", tts = "mock";
    std::string mnn_config, tts_model, tts_tokens, tts_lexicon, tts_rule_fars;
    std::string mnn_backend; // empty = keep model config.json backend_type
    std::string strategies = "P0,P1,P2,P3";
    std::string tts_lines;
    int deadline_ms = 4000, max_tokens = 64, tts_threads = 2;
    int mnn_threads = 0;     // 0 = keep model config.json thread_num
    int rounds = 1, warmup_rounds = 0;
    bool burst = false;
    int switch_session_after = -1; // burst: flip active session after K submissions
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
                "--llm mock|mnn --tts mock|sherpa --mnn-config PATH --mnn-backend cpu|opencl "
                "--mnn-threads N --max-tokens N --strategies P0,P1,P2,P3 --rounds N "
                "--warmup-rounds N --burst --switch-session-after K "
                "--tts-lines FILE --tts-model PATH --tts-tokens PATH --tts-lexicon PATH "
                "--tts-rule-fars PATH --tts-threads N --deadline-ms N\n";
            std::exit(0);
        }
        if (k == "--burst") { o.burst = true; continue; }
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + k);
        std::string v = argv[++i];
        if (k == "--cases") o.cases = v;
        else if (k == "--output") o.output = v;
        else if (k == "--run-id") o.run_id = v;
        else if (k == "--llm") o.llm = v;
        else if (k == "--tts") o.tts = v;
        else if (k == "--mnn-config") o.mnn_config = v;
        else if (k == "--mnn-backend") o.mnn_backend = v;
        else if (k == "--mnn-threads") o.mnn_threads = std::stoi(v);
        else if (k == "--max-tokens") o.max_tokens = std::stoi(v);
        else if (k == "--strategies") o.strategies = v;
        else if (k == "--rounds") o.rounds = std::stoi(v);
        else if (k == "--warmup-rounds") o.warmup_rounds = std::stoi(v);
        else if (k == "--switch-session-after") o.switch_session_after = std::stoi(v);
        else if (k == "--tts-lines") o.tts_lines = v;
        else if (k == "--tts-model") o.tts_model = v;
        else if (k == "--tts-tokens") o.tts_tokens = v;
        else if (k == "--tts-lexicon") o.tts_lexicon = v;
        else if (k == "--tts-rule-fars") o.tts_rule_fars = v;
        else if (k == "--tts-threads") o.tts_threads = std::stoi(v);
        else if (k == "--deadline-ms") o.deadline_ms = std::stoi(v);
        else throw std::invalid_argument("unknown option " + k);
    }
    if (o.deadline_ms < 1 || o.max_tokens < 1 || o.tts_threads < 1 ||
        o.rounds < 1 || o.warmup_rounds < 0 || o.mnn_threads < 0)
        throw std::invalid_argument("invalid numeric option");
    if (o.run_id.empty() || !std::all_of(o.run_id.begin(), o.run_id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        }))
        throw std::invalid_argument("run-id must contain ASCII letters, digits, - or _");
    return o;
}
std::vector<std::string> split_fields(const std::string& line, char sep) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    for (std::size_t p = 0; p <= line.size(); ++p) {
        if (p == line.size() || line[p] == sep) {
            fields.push_back(line.substr(begin, p - begin));
            begin = p + 1;
        }
    }
    return fields;
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
        auto fields = split_fields(line, '\t');
        Case c;
        if (fields.size() == 6) {
            c = Case{fields[0], fields[2], fields[3], fields[4], fields[5],
                     std::stoi(fields[1]), {}, {}, {}, {}, {}, {}};
        } else if (fields.size() == 12) {
            c = Case{fields[0], fields[2], fields[3], fields[4], fields[5], std::stoi(fields[1]),
                     fields[6], fields[7], fields[8], fields[9], fields[10], fields[11]};
        } else {
            throw std::runtime_error("expected 6 or 12 TSV columns on line " + std::to_string(lineno));
        }
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
std::vector<TtsLine> load_tts_lines(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open tts lines: " + path);
    std::vector<TtsLine> lines;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto fields = split_fields(line, '\t');
        if (fields.size() != 4)
            throw std::runtime_error("expected 4 TSV columns on line " + std::to_string(lineno));
        TtsLine l{fields[0], fields[1], fields[3], std::stoi(fields[2])};
        if (l.speaker < 1 || l.speaker > 3 || l.text.empty() || l.id.empty() ||
            !std::all_of(l.id.begin(), l.id.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '_' || ch == '-';
            }))
            throw std::runtime_error("invalid TTS line on line " + std::to_string(lineno));
        lines.push_back(std::move(l));
    }
    if (lines.empty()) throw std::runtime_error("no tts lines found");
    return lines;
}
double milliseconds(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}
std::uint64_t peak_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS p{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &p, sizeof(p))) return p.PeakWorkingSetSize;
#endif
    return 0;
}
// Decode UTF-8 to code points (invalid bytes become U+FFFD, never throw).
std::u32string to_codepoints(const std::string& s) {
    std::u32string out;
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        char32_t cp = U'\uFFFD';
        if (b < 0x80) cp = b;
        else if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) { len = 2;
            cp = static_cast<char32_t>((b & 0x1F) << 6); }
        else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) { len = 3;
            cp = static_cast<char32_t>((b & 0x0F) << 12); }
        else if ((b & 0xF8) == 0xF0 && i + 3 < s.size()) { len = 4;
            cp = static_cast<char32_t>((b & 0x07) << 18); }
        bool ok = len > 1;
        for (std::size_t k = 1; k < len; ++k) {
            unsigned char cb = static_cast<unsigned char>(s[i + k]);
            if ((cb & 0xC0) != 0x80) { ok = false; break; }
            cp |= static_cast<char32_t>((cb & 0x3F) << (6 * (len - 1 - k)));
        }
        if (!ok) { cp = U'\uFFFD'; len = 1; }
        out.push_back(cp);
        i += len;
    }
    return out;
}
// Longest common substring length in code points (echo/duplication heuristic).
std::size_t lcs_codepoints(const std::u32string& a, const std::u32string& b) {
    if (a.empty() || b.empty()) return 0;
    std::vector<std::size_t> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
    std::size_t best = 0;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        for (std::size_t j = 1; j <= b.size(); ++j) {
            cur[j] = (a[i - 1] == b[j - 1]) ? prev[j - 1] + 1 : 0;
            best = std::max(best, cur[j]);
        }
        std::swap(prev, cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    return best;
}
int utf8_hits(const std::string& text, const std::string& keyword) {
    if (keyword.empty()) return 0;
    int n = 0;
    for (std::size_t p = text.find(keyword); p != std::string::npos; p = text.find(keyword, p + keyword.size())) ++n;
    return n;
}
std::string strategy_prompt(const Case& c, char s) {
    switch (s) {
    case '0': // P0: persona only, no event facts
        return "你是单机麻将游戏中的虚构牌友" + c.persona_key + "。请说一句口语化的中文角色台词。";
    case '1': // P1: persona + current public event (canonical production prompt)
        return c.prompt;
    case '2': // P2: persona + public board state + dialogue-history summary
        return "你是单机麻将游戏中的虚构牌友" + c.persona_key +
               "。当前公开牌局状态：" + c.fact +
               "。历史对话摘要：" + (c.history.empty() ? "无（本局刚开始）" : c.history) +
               "。只依据公开状态说一句6到20字的口语化台词，不要猜测未公开信息，不要解释。";
    case '3': // P3: short constraint + structured event
        return "角色：" + c.persona_key + "。事件：" + c.structured +
               "。要求：只说一句6到20字口语台词，不猜未公开牌，不自称AI。";
    default:
        throw std::invalid_argument("unknown strategy");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        auto opt = parse(argc, argv);
        fs::path out = fs::path(opt.output) / opt.run_id;
        fs::create_directories(out);
        std::ofstream log(out / "results.jsonl", std::ios::binary | std::ios::trunc);
        if (!log) throw std::runtime_error("cannot open results.jsonl");
        std::unique_ptr<ILlmBackend> llm;
        std::unique_ptr<ITtsBackend> tts;
        auto init_start = Clock::now();
        if (opt.llm == "mock") llm = std::make_unique<MockLlm>(10ms);
#ifdef AIMJ_WITH_MNN
        else if (opt.llm == "mnn")
            llm = make_mnn_backend(opt.mnn_config, opt.max_tokens, opt.mnn_backend, opt.mnn_threads);
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

        // ---- TTS-only battery: fixed voice fixtures through the TTS backend ----
        if (!opt.tts_lines.empty()) {
            auto lines = load_tts_lines(opt.tts_lines);
            log << "{\"type\":\"run\",\"schema\":2,\"run_id\":" << json(opt.run_id)
                << ",\"mode\":" << json(real ? "real" : mixed ? "mixed" : "mock")
                << ",\"battery\":\"tts_lines\",\"llm_provider\":" << json(opt.llm)
                << ",\"tts_provider\":" << json(opt.tts)
                << ",\"tts_lines\":" << lines.size()
                << ",\"provider_init_ms\":" << init_ms
                << ",\"first_audible_ms\":null}\n";
            std::size_t ok = 0, failed = 0;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const auto& l = lines[i];
                auto start = Clock::now();
                std::string status = "ok";
                AudioBuffer audio;
                try {
                    audio = tts->synthesize(l.text, l.speaker);
                    if (audio.samples.empty() || audio.sample_rate <= 0)
                        throw std::runtime_error("empty audio");
                } catch (...) { status = "error"; }
                double synth_ms = milliseconds(Clock::now() - start);
                std::string wav;
                double audio_s = 0;
                if (status == "ok") {
                    wav = l.id + ".wav";
                    try {
                        write_pcm16_wav((out / wav).string(), audio);
                        audio_s = static_cast<double>(audio.samples.size()) / audio.sample_rate;
                        ++ok;
                    } catch (...) { status = "error"; wav.clear(); ++failed; --ok; }
                } else ++failed;
                log << std::fixed << std::setprecision(3)
                    << "{\"type\":\"tts_line\",\"id\":" << json(l.id)
                    << ",\"category\":" << json(l.category)
                    << ",\"speaker\":" << l.speaker
                    << ",\"text\":" << json(l.text)
                    << ",\"sid\":" << l.speaker - 1
                    << ",\"status\":" << json(status)
                    << ",\"wav\":" << json(wav)
                    << ",\"sample_rate\":" << audio.sample_rate
                    << ",\"pcm_samples\":" << audio.samples.size()
                    << ",\"audio_s\":" << audio_s
                    << ",\"synth_ms\":" << synth_ms
                    << ",\"rtf\":" << (audio_s > 0 ? (synth_ms / 1000.0) / audio_s : 0.0)
                    << ",\"first_audible_ms\":null}\n";
                std::cout << '[' << (i + 1) << '/' << lines.size() << "] " << l.id
                          << " " << status << " synth_ms=" << synth_ms
                          << " audio_s=" << audio_s << '\n';
            }
            log << "{\"type\":\"summary\",\"lines\":" << lines.size()
                << ",\"ok_with_wav\":" << ok << ",\"failed\":" << failed
                << ",\"peak_rss_bytes\":" << peak_rss_bytes()
                << ",\"quality_human_graded\":false,\"first_audible_measured\":false}\n";
            log.flush();
            std::cout << "Results: " << (out / "results.jsonl").string()
                      << "; WAV=" << ok << "; failures=" << failed << '\n';
            return failed == 0 ? 0 : 2;
        }

        auto cases = load_cases(opt.cases);
        std::vector<char> strategies;
        for (char c : opt.strategies)
            if (c == '0' || c == '1' || c == '2' || c == '3') strategies.push_back(c);
        if (strategies.empty()) throw std::invalid_argument("no valid strategy in --strategies");
        log << "{\"type\":\"run\",\"schema\":2,\"run_id\":" << json(opt.run_id)
            << ",\"mode\":" << json(real ? "real" : mixed ? "mixed" : "mock")
            << ",\"battery\":\"dialogue\",\"llm_provider\":" << json(opt.llm)
            << ",\"tts_provider\":" << json(opt.tts)
            << ",\"mnn_backend\":" << json(opt.mnn_backend.empty() ? "config_default" : opt.mnn_backend)
            << ",\"mnn_threads\":" << (opt.mnn_threads > 0 ? opt.mnn_threads : 0)
            << ",\"strategies\":" << json(opt.strategies)
            << ",\"cases\":" << cases.size()
            << ",\"rounds\":" << opt.rounds
            << ",\"warmup_rounds\":" << opt.warmup_rounds
            << ",\"max_new_tokens\":" << opt.max_tokens
            << ",\"deadline_ms\":" << opt.deadline_ms
            << ",\"burst\":" << (opt.burst ? "true" : "false")
            << ",\"switch_session_after\":" << opt.switch_session_after
            << ",\"provider_init_ms\":" << init_ms
            << ",\"first_audible_ms\":null}\n";
        AiRuntime runtime(std::move(llm), std::move(tts), 2);
        struct Work { const Case* c; char strategy; int round; bool warmup; int session; };
        std::vector<Work> work;
        for (int w = 0; w < opt.warmup_rounds; ++w)
            for (char s : strategies)
                for (const auto& c : cases) work.push_back({&c, s, 0, true, 1});
        for (int r = 1; r <= opt.rounds; ++r)
            for (char s : strategies)
                for (const auto& c : cases) work.push_back({&c, s, r, false, 1});
        std::size_t submitted_total = work.size();
        std::size_t switch_idx = opt.switch_session_after >= 0
            ? static_cast<std::size_t>(opt.switch_session_after) : work.size();
        std::map<int, Work*> by_event;
        int event_seq = 0;
        std::size_t offered = 0, rejected = 0;
        if (opt.burst) {
            // Scenario battery: fire every request without waiting, letting the
            // bounded queue + deadline machinery reject/expire; then drain.
            for (std::size_t i = 0; i < work.size(); ++i) {
                auto& w = work[i];
                int session = i < switch_idx ? 1 : 2;
                w.session = session;
                DialogueRequest req{};
                req.session_id = session;
                req.event_id = ++event_seq;
                const int eid = req.event_id;
                req.speaker = w.c->speaker;
                req.persona = w.c->persona_key;
                req.prompt = strategy_prompt(*w.c, w.strategy);
                req.fallback_line = "（暂时无语音）";
                req.created = Clock::now();
                req.deadline = req.created + std::chrono::milliseconds(opt.deadline_ms);
                by_event[eid] = &w;
                if (runtime.submit(std::move(req))) { ++offered; }
                else { ++rejected; by_event.erase(eid); }
                if (i + 1 == switch_idx && opt.switch_session_after >= 0)
                    runtime.set_active_session(2);
            }
        }
        std::size_t done = 0, ok = 0, failed = 0;
        std::map<std::string, std::string> first_text; // case+strategy -> first formal text
        auto drain = [&](std::optional<SpeechResult> r) {
            if (!r) return;
            auto it = by_event.find(r->event_id);
            if (it == by_event.end()) return;
            const Work& w = *it->second;
            const Case& c = *w.c;
            by_event.erase(it);
            ++done;
            std::string status = r->status;
            std::string wav;
            double audio_s = 0, rtf = 0;
            bool formal = !w.warmup;
            if (status == "ok" && !r->audio.samples.empty() && r->audio.sample_rate > 0) {
                // Warmup rounds must not clobber formal-round WAVs; name by round.
                wav = c.id + "_P" + std::string(1, w.strategy) +
                      (w.warmup ? "_w" : "_r" + std::to_string(w.round)) + ".wav";
                try {
                    write_pcm16_wav((out / wav).string(), r->audio);
                    audio_s = static_cast<double>(r->audio.samples.size()) / r->audio.sample_rate;
                    rtf = (r->tts_ms / 1000.0) / audio_s;
                    if (formal) ++ok;
                } catch (...) { wav.clear(); status = "wav_error"; if (formal) ++failed; }
            } else if (formal) ++failed;
            std::u32string cps = to_codepoints(r->text);
            std::u32string pcps = to_codepoints(strategy_prompt(c, w.strategy));
            std::size_t echo = lcs_codepoints(cps, pcps);
            std::string key = c.id + "_P" + std::string(1, w.strategy);
            std::string repeat_of;
            if (formal) {
                auto fit = first_text.find(key);
                if (fit == first_text.end()) first_text[key] = r->text;
                else if (fit->second == r->text) repeat_of = fit->second;
            }
            log << std::fixed << std::setprecision(3)
                << "{\"type\":\"case\",\"id\":" << json(c.id)
                << ",\"category\":" << json(c.category)
                << ",\"strategy\":\"P" << w.strategy << "\""
                << ",\"round\":" << w.round
                << ",\"warmup\":" << (w.warmup ? "true" : "false")
                << ",\"session_id\":" << r->session_id
                << ",\"speaker\":" << c.speaker
                << ",\"persona_key\":" << json(c.persona_key)
                << ",\"expected_review_hint\":" << json(c.expected)
                << ",\"forbidden_review_hint\":" << json(c.forbidden)
                << ",\"required_kw\":" << json(c.required_kw)
                << ",\"forbidden_kw\":" << json(c.forbidden_kw)
                << ",\"required_hits\":{";
            {
                bool first = true;
                for (const auto& kw : split_fields(c.required_kw, ';')) {
                    if (kw.empty()) continue;
                    if (!first) log << ",";
                    first = false;
                    log << json(kw) << ":" << utf8_hits(r->text, kw);
                }
            }
            log << "},\"forbidden_hits\":[";
            {
                bool first = true;
                for (const auto& kw : split_fields(c.forbidden_kw, ';')) {
                    if (kw.empty()) continue;
                    int hits = utf8_hits(r->text, kw);
                    if (hits > 0) {
                        if (!first) log << ",";
                        first = false;
                        log << json(kw);
                    }
                }
            }
            log << "]"
                << ",\"status\":" << json(status)
                << ",\"generated_text\":" << json(r->text)
                << ",\"text_codepoints\":" << cps.size()
                << ",\"prompt_overlap_codepoints\":" << echo
                << ",\"repeat_of_first_round\":" << (repeat_of.empty() ? "false" : "true")
                << ",\"audio_wav\":" << json(wav)
                << ",\"sample_rate\":" << (status == "ok" ? r->audio.sample_rate : 0)
                << ",\"audio_s\":" << audio_s
                << ",\"queue_llm_ms\":" << r->queue_llm_ms
                << ",\"llm_ms\":" << r->llm_ms
                << ",\"queue_tts_ms\":" << r->queue_tts_ms
                << ",\"llm_ttfc_ms\":" << r->llm_usage.ttfc_ms
                << ",\"prompt_tokens\":" << r->llm_usage.prompt_tokens
                << ",\"generated_tokens\":" << r->llm_usage.generated_tokens
                << ",\"prefill_ms\":" << r->llm_usage.prefill_ms
                << ",\"decode_ms\":" << r->llm_usage.decode_ms
                << ",\"tts_ms\":" << r->tts_ms
                << ",\"tts_rtf\":" << rtf
                << ",\"end_to_end_pcm_ms\":" << r->end_to_end_ms
                << ",\"first_audible_ms\":null}\n";
            std::cout << '[' << done << '/' << submitted_total << "] " << c.id
                      << " P" << w.strategy << (w.warmup ? "(w)" : "")
                      << " r" << w.round << " " << status << " "
                      << (status == "ok" || status == "wav_error" ? r->text : "(no result)") << '\n';
        };
        if (opt.burst) {
            const auto until = Clock::now() + std::chrono::milliseconds(opt.deadline_ms + 180000);
            while (!by_event.empty() && Clock::now() < until) {
                if (auto r = runtime.poll()) drain(std::move(r));
                else std::this_thread::sleep_for(1ms);
            }
        } else {
            for (auto& w : work) {
                DialogueRequest req{};
                req.session_id = w.session;
                req.event_id = ++event_seq;
                const int eid = req.event_id;
                req.speaker = w.c->speaker;
                req.persona = w.c->persona_key;
                req.prompt = strategy_prompt(*w.c, w.strategy);
                req.fallback_line = "（暂时无语音）";
                req.created = Clock::now();
                req.deadline = req.created + std::chrono::milliseconds(opt.deadline_ms);
                by_event[eid] = &w;
                bool submitted = runtime.submit(std::move(req));
                if (submitted) { ++offered; }
                else { ++rejected; by_event.erase(eid); ++done;
                    log << "{\"type\":\"case\",\"id\":" << json(w.c->id)
                        << ",\"strategy\":\"P" << w.strategy << "\",\"round\":" << w.round
                        << ",\"warmup\":" << (w.warmup ? "true" : "false")
                        << ",\"status\":\"queue_rejected\",\"generated_text\":\"\""
                        << ",\"first_audible_ms\":null}\n"; continue; }
                std::optional<SpeechResult> r;
                const auto until = req.created + std::chrono::milliseconds(opt.deadline_ms + 3000);
                while (Clock::now() < until) {
                    if ((r = runtime.poll())) break;
                    std::this_thread::sleep_for(1ms);
                }
                drain(std::move(r));
            }
        }
        auto stats = runtime.counters();
        log << "{\"type\":\"summary\",\"work_items\":" << submitted_total
            << ",\"offered\":" << offered << ",\"submit_rejected\":" << rejected
            << ",\"resolved\":" << done << ",\"unresolved\":" << by_event.size()
            << ",\"ok_with_wav\":" << ok << ",\"failed_or_missing\":" << failed
            << ",\"runtime_accepted\":" << stats.accepted
            << ",\"runtime_queue_rejected\":" << stats.queue_rejected
            << ",\"runtime_expired\":" << stats.expired
            << ",\"runtime_stale\":" << stats.stale
            << ",\"runtime_failed\":" << stats.failed
            << ",\"peak_rss_bytes\":" << peak_rss_bytes()
            << ",\"quality_human_graded\":false,\"first_audible_measured\":false}\n";
        log.flush();
        std::cout << "Results: " << (out / "results.jsonl").string()
                  << "; offered=" << offered << " submit_rejected=" << rejected
                  << "; WAV=" << ok << "; failures=" << failed
                  << "; peak_rss_bytes=" << peak_rss_bytes() << '\n';
        return failed == 0 ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
