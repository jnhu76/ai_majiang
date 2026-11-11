#include "aimj/runtime.hpp"
#include "aimj/textscore.hpp"
#include "aimj/wav.hpp"
#include "sha256.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <winver.h>
#endif

using namespace aimj;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
// Schema 3 evidence record: every dialogue row carries the verbatim prompt, the
// model's own text even when a fallback replaced it, the provider verdict and
// all timing boundaries. mock/mixed rows are structurally separated from real
// measurements by `mode` + `usage_valid`, never by reading numbers as real.
constexpr int kSchema = 3;
constexpr const char* kFirstAudibleReason =
    "playback layer is not wired into this build (Axmol play2d absent); not estimated";
constexpr const char* kGuard = "不要猜任何未公开手牌、胜负或分数。";
constexpr const char* kPersonaTemplate = "你是单机麻将游戏中的虚构牌友{persona}。";
struct Persona { const char* key; const char* name; const char* traits; };
constexpr Persona kPersonas[] = {
    {"嘟嘟", "嘟嘟", "调皮，喜欢叠词"},
    {"急急哥", "急急哥", "急性子但不辱骂"},
    {"淡定猫", "淡定猫", "冷幽默"},
};
constexpr const char* kP0Template =
    "你是单机麻将游戏中的虚构牌友{persona}。请说一句口语化中文角色台词，不要解释、不要自称AI。";
constexpr const char* kP2Template =
    "你是单机麻将游戏中的虚构牌友{persona}。当前公开牌局状态：{fact}。历史对话摘要：{history}。"
    "只依据公开状态说一句6到20字的口语化台词，不要猜未公开信息，不要解释。";
constexpr const char* kP3Template =
    "角色：{persona}。事件：{structured}。要求：只说一句6到20字口语台词，不猜未公开牌，不自称AI。";
constexpr const char* kTemplateModeTemplate =
    "你是单机麻将游戏中的虚构牌友{persona}。规则引擎负责所有合法操作与结算，你只能根据以下已公开事实"
    "说一句口语化中文台词（建议6到20字，不要解释、不要自称AI）。当前公开事实：{fact}。"
    "历史对话摘要：{history}。只依据这些公开事实说话，不得新增事实。";
constexpr const char* kStrictRetrySuffix =
    "上一次回答没有满足约束：{reasons}。请只改写措辞，仍只依据同样的事实，输出6到20字的角色台词。";

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
std::string jnum(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << v;
    return os.str();
}
std::string jnull(bool valid, double v) { return valid ? jnum(v) : "null"; }
std::string jbool(bool v) { return v ? "true" : "false"; }
std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    std::size_t p = 0;
    while ((p = text.find(from, p)) != std::string::npos) {
        text.replace(p, from.size(), to);
        p += to.size();
    }
    return text;
}

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
    int sid{-1}; // explicit 5th column wins over --tts-sids/speaker-1
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
    std::string tts_sids; // r1,r2,r3 -> sid for speakers 1..3
    std::string battery;  // dialogue|tts_lines|scenarios|hybrid
    std::string scenario; // s1..s6 when battery=scenarios
    std::string dialogue_mode = "free"; // free|template|cache
    std::string approach;               // A|B|C, derived from mode when empty
    std::string bank_manifest, bank_dir;
    std::string config_name;
    std::string llm_sdk_label, tts_sdk_label; // caller-known build label, e.g. commit
    int deadline_ms = 4000, max_tokens = 64, tts_threads = 2;
    int mnn_threads = 0; // 0 = keep model config.json thread_num
    int queue = 2;
    int inject_after = -1; // -1 disabled; 0-based generate() call that throws once
    int rounds = 1, warmup_rounds = 0;
    bool burst = false;
    int switch_session_after = -1; // burst: flip active session after K submissions
    bool chain_history = false;
    bool force = false;
    bool allow_backend_fallback = false;
};
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
int parse_int(const std::string& text, const std::string& what) {
    try {
        std::size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used != text.size()) throw std::invalid_argument("trailing");
        return value;
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid " + what + ": " + text);
    }
}
Opt parse(int argc, char** argv) {
    Opt o;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--help") {
            std::cout
                << "aimj_dialogue_probe --cases FILE --output DIR --run-id ID --battery "
                   "dialogue|tts_lines|scenarios|hybrid --scenario s1..s6\n"
                   "  --llm mock|mnn --tts mock|sherpa --mnn-config PATH --mnn-backend cpu|opencl "
                   "--mnn-threads N --allow-backend-fallback\n"
                   "  --max-tokens N --strategies P0,P1,P2,P3 --rounds N --warmup-rounds N --queue N "
                   "--deadline-ms N\n"
                   "  --dialogue-mode free|template|cache --bank-manifest FILE --bank-dir DIR --approach A|B|C\n"
                   "  --burst --switch-session-after K --chain-history --inject-llm-error-after N\n"
                   "  --tts-lines FILE --tts-sids r1,r2,r3 --tts-model PATH --tts-tokens PATH "
                   "--tts-lexicon PATH --tts-rule-fars PATH --tts-threads N\n"
                   "  --config-name NAME --llm-sdk-label TEXT --tts-sdk-label TEXT --force\n";
            std::exit(0);
        }
        if (k == "--burst") { o.burst = true; continue; }
        if (k == "--chain-history") { o.chain_history = true; continue; }
        if (k == "--force") { o.force = true; continue; }
        if (k == "--allow-backend-fallback") { o.allow_backend_fallback = true; continue; }
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + k);
        std::string v = argv[++i];
        if (k == "--cases") o.cases = v;
        else if (k == "--output") o.output = v;
        else if (k == "--run-id") o.run_id = v;
        else if (k == "--llm") o.llm = v;
        else if (k == "--tts") o.tts = v;
        else if (k == "--mnn-config") o.mnn_config = v;
        else if (k == "--mnn-backend") o.mnn_backend = v;
        else if (k == "--mnn-threads") o.mnn_threads = parse_int(v, "--mnn-threads");
        else if (k == "--max-tokens") o.max_tokens = parse_int(v, "--max-tokens");
        else if (k == "--strategies") o.strategies = v;
        else if (k == "--rounds") o.rounds = parse_int(v, "--rounds");
        else if (k == "--warmup-rounds") o.warmup_rounds = parse_int(v, "--warmup-rounds");
        else if (k == "--switch-session-after") o.switch_session_after = parse_int(v, "--switch-session-after");
        else if (k == "--queue") o.queue = parse_int(v, "--queue");
        else if (k == "--tts-lines") o.tts_lines = v;
        else if (k == "--tts-sids") o.tts_sids = v;
        else if (k == "--tts-model") o.tts_model = v;
        else if (k == "--tts-tokens") o.tts_tokens = v;
        else if (k == "--tts-lexicon") o.tts_lexicon = v;
        else if (k == "--tts-rule-fars") o.tts_rule_fars = v;
        else if (k == "--tts-threads") o.tts_threads = parse_int(v, "--tts-threads");
        else if (k == "--deadline-ms") o.deadline_ms = parse_int(v, "--deadline-ms");
        else if (k == "--inject-llm-error-after") o.inject_after = parse_int(v, "--inject-llm-error-after");
        else if (k == "--battery") o.battery = v;
        else if (k == "--scenario") o.scenario = v;
        else if (k == "--dialogue-mode") o.dialogue_mode = v;
        else if (k == "--approach") o.approach = v;
        else if (k == "--bank-manifest") o.bank_manifest = v;
        else if (k == "--bank-dir") o.bank_dir = v;
        else if (k == "--config-name") o.config_name = v;
        else if (k == "--llm-sdk-label") o.llm_sdk_label = v;
        else if (k == "--tts-sdk-label") o.tts_sdk_label = v;
        else throw std::invalid_argument("unknown option " + k);
    }
    if (o.deadline_ms < 1 || o.max_tokens < 1 || o.tts_threads < 1 || o.queue < 1 ||
        o.rounds < 1 || o.warmup_rounds < 0 || o.mnn_threads < 0 || o.inject_after < -1)
        throw std::invalid_argument("invalid numeric option");
    if (o.run_id.empty() || !std::all_of(o.run_id.begin(), o.run_id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        }))
        throw std::invalid_argument("run-id must contain ASCII letters, digits, - or _");
    if (o.llm != "mock" && o.llm != "mnn") throw std::invalid_argument("unknown --llm provider: " + o.llm);
    if (o.tts != "mock" && o.tts != "sherpa") throw std::invalid_argument("unknown --tts provider: " + o.tts);
    if (o.battery.empty()) o.battery = o.tts_lines.empty() ? "dialogue" : "tts_lines";
    if (o.battery != "dialogue" && o.battery != "tts_lines" && o.battery != "scenarios" && o.battery != "hybrid")
        throw std::invalid_argument("unknown --battery: " + o.battery);
    if (o.dialogue_mode != "free" && o.dialogue_mode != "template" && o.dialogue_mode != "cache")
        throw std::invalid_argument("unknown --dialogue-mode: " + o.dialogue_mode);
    if (o.battery == "tts_lines" && o.tts_lines.empty()) throw std::invalid_argument("--tts-lines is required");
    if (o.battery == "scenarios" && o.scenario.empty()) throw std::invalid_argument("--scenario s1..s6 is required");
    if (o.dialogue_mode == "cache" && o.bank_manifest.empty())
        throw std::invalid_argument("--dialogue-mode cache requires --bank-manifest");
    if (o.approach.empty()) {
        o.approach = o.dialogue_mode == "free" ? "A" : o.dialogue_mode == "template" ? "B" : "C";
    }
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
        auto fields = split_fields(line, '\t');
        Case c;
        if (fields.size() == 6) {
            c = Case{fields[0], fields[2], fields[3], fields[4], fields[5],
                     parse_int(fields[1], "speaker"), {}, {}, {}, {}, {}, {}};
        } else if (fields.size() == 12) {
            c = Case{fields[0], fields[2], fields[3], fields[4], fields[5],
                     parse_int(fields[1], "speaker"), fields[6], fields[7], fields[8],
                     fields[9], fields[10], fields[11]};
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
        if (fields.size() != 4 && fields.size() != 5)
            throw std::runtime_error("expected 4 or 5 TSV columns on line " + std::to_string(lineno));
        TtsLine l{fields[0], fields[1], fields[3], parse_int(fields[2], "speaker"),
                  fields.size() == 5 ? parse_int(fields[4], "sid") : -1};
        if (l.speaker < 1 || l.speaker > 3 || l.text.empty() || l.id.empty() ||
            (l.sid < -1 || l.sid > 15) ||
            !std::all_of(l.id.begin(), l.id.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '_' || ch == '-';
            }))
            throw std::runtime_error("invalid TTS line on line " + std::to_string(lineno));
        for (const auto& previous : lines)
            if (previous.id == l.id) throw std::runtime_error("duplicate tts line id " + l.id);
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
std::string read_text_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
std::string json_string_field(const std::string& text, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto p = text.find(needle);
    if (p == std::string::npos) return {};
    p = text.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = text.find('"', p + 1);
    if (p == std::string::npos) return {};
    const auto end = text.find('"', p + 1);
    if (end == std::string::npos) return {};
    return text.substr(p + 1, end - p - 1);
}
double json_number_field(const std::string& text, const std::string& key, double fallback) {
    const std::string needle = "\"" + key + "\"";
    auto p = text.find(needle);
    if (p == std::string::npos) return fallback;
    p = text.find(':', p + needle.size());
    if (p == std::string::npos) return fallback;
    try { return std::stod(text.substr(p + 1)); } catch (const std::exception&) { return fallback; }
}
std::string persona_label(const std::string& key) {
    for (const auto& p : kPersonas)
        if (key == p.key) return std::string(p.name) + "（" + p.traits + "）";
    return key.empty() ? "无名牌友" : key;
}
std::string persona_traits(const std::string& key) {
    for (const auto& p : kPersonas)
        if (key == p.key) return p.traits;
    return "普通牌友";
}
std::string strategy_prompt(const Case& c, char s) {
    const std::string persona = persona_label(c.persona_key);
    switch (s) {
    case '0': // P0: persona only, no event facts
        return replace_all(kP0Template, "{persona}", persona);
    case '1': // P1: persona + current public event (canonical production prompt)
        return c.prompt;
    case '2': { // P2: persona + public board state + dialogue-history summary
        std::string prompt = replace_all(kP2Template, "{persona}", persona);
        prompt = replace_all(prompt, "{fact}", c.fact.empty() ? c.prompt : c.fact);
        prompt = replace_all(prompt, "{history}", c.history.empty() ? "无（本局刚开始）" : c.history);
        return prompt;
    }
    case '3': // P3: short constraint + structured event
        return replace_all(replace_all(kP3Template, "{persona}", persona),
                           "{structured}", c.structured.empty() ? c.fact : c.structured);
    default:
        throw std::invalid_argument("unknown strategy");
    }
}
// Dialogue mode B: the same facts as P1, rendered through a fixed template, with
// a machine check after generation and one constrained re-prompt on violation.
std::string template_prompt(const Case& c) {
    std::string prompt = replace_all(kTemplateModeTemplate, "{persona}", persona_label(c.persona_key));
    prompt = replace_all(prompt, "{fact}", c.fact.empty() ? c.prompt : c.fact);
    prompt = replace_all(prompt, "{history}", c.history.empty() ? "无（本局刚开始）" : c.history);
    return prompt + kGuard;
}
std::string strict_retry_prompt(const Case& c, const std::vector<std::string>& reasons) {
    std::string joined;
    for (const auto& r : reasons) {
        if (!joined.empty()) joined += "；";
        joined += r;
    }
    return template_prompt(c) + replace_all(kStrictRetrySuffix, "{reasons}", joined);
}
// TTS input hygiene: real models sometimes emit leading spaces or control
// markers. The fixture text stays on record as tts_raw_text; what the backend
// actually receives is tts_input_text.
std::string sanitize_tts_text(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if (c < 0x20) {
            if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
            continue;
        }
        if (c == 0x7f) continue;
        out.push_back(static_cast<char>(c));
    }
    const auto begin = out.find_first_not_of(' ');
    if (begin == std::string::npos) return {};
    const auto end = out.find_last_not_of(' ');
    return out.substr(begin, end - begin + 1);
}
#if defined(_WIN32)
std::wstring utf16(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string file_version(const std::string& path) {
    if (path.empty()) return {};
    DWORD handle = 0;
    const std::wstring wide = utf16(path);
    const DWORD size = GetFileVersionInfoSizeW(wide.c_str(), &handle);
    if (size == 0) return {};
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(wide.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) || !info)
        return {};
    std::ostringstream os;
    os << HIWORD(info->dwFileVersionMS) << '.' << LOWORD(info->dwFileVersionMS) << '.'
       << HIWORD(info->dwFileVersionLS) << '.' << LOWORD(info->dwFileVersionLS);
    return os.str();
}
std::string loaded_module_file(const wchar_t* name) {
    HMODULE handle = GetModuleHandleW(name);
    if (!handle) return {};
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(handle, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(n), out.data(), bytes, nullptr, nullptr);
    return out;
}
bool file_contains(const std::string& path, const char* needle) {
    const std::string data = read_text_file(path);
    return data.find(needle) != std::string::npos;
}
#endif
#if !defined(_WIN32)
// Windows reads the DLL VERSIONINFO resource; other platforms have no
// equivalent here, so callers fall back to the path/label.
std::string file_version(const std::string&) { return {}; }
#endif
// SDK archives are named ...-v2.0.1-win-x64...; use that only as a labeled
// fallback when the DLL has no VERSIONINFO resource, never as a guess.
// Pure string parsing, shared by every platform.
std::string version_from_path(const std::string& path) {
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        if (path[i] != 'v' || !std::isdigit(static_cast<unsigned char>(path[i + 1]))) continue;
        std::size_t end = i + 1;
        int dots = 0;
        while (end < path.size() &&
               (std::isdigit(static_cast<unsigned char>(path[end])) || path[end] == '.')) {
            if (path[end] == '.') ++dots;
            ++end;
        }
        if (dots >= 2) return "v" + path.substr(i + 1, end - i - 1);
    }
    return {};
}
struct SidMap {
    std::array<int, 3> sid{0, 1, 2};
    int for_speaker(int speaker) const {
        const int index = std::clamp(speaker, 1, 3) - 1;
        return sid[static_cast<std::size_t>(index)];
    }
};
SidMap parse_sids(const std::string& text) {
    SidMap map;
    if (text.empty()) return map;
    const auto parts = split_fields(text, ',');
    if (parts.size() != 3) throw std::invalid_argument("--tts-sids expects r1,r2,r3");
    for (std::size_t i = 0; i < 3; ++i) {
        const int sid = parse_int(parts[i], "--tts-sids");
        if (sid < 0 || sid > 15) throw std::invalid_argument("--tts-sids values must be 0..15");
        map.sid[i] = sid;
    }
    return map;
}
// Single-shot fault injection at a fixed 0-based generate() call. It throws
// once, so the run proves recovery after a model failure (s6/calibration).
class FaultInjectLlm final : public ILlmBackend {
public:
    FaultInjectLlm(std::unique_ptr<ILlmBackend> inner, int after)
        : inner_(std::move(inner)), after_(after) {}
    std::string generate(const std::string& prompt, int speaker) override {
        const int call = calls_.fetch_add(1);
        if (call == after_) {
            inject_usage_pending_.store(true);
            throw std::runtime_error("injected llm fault after " + std::to_string(after_) + " calls");
        }
        return inner_->generate(prompt, speaker);
    }
    LlmUsage last_usage() const override {
        // The injected call never reaches the model; report the status an
        // INTERNAL_ERROR failure carries (MNN LlmStatus::INTERNAL_ERROR=4) so
        // calibration can separate errors from truncation. The row keeps
        // injected_fault=true, so the simulated origin stays explicit.
        if (inject_usage_pending_.exchange(false)) {
            LlmUsage usage{};
            usage.status_code = 4;
            return usage;
        }
        return inner_->last_usage();
    }
    std::string name() const override { return inner_->name(); }
private:
    std::unique_ptr<ILlmBackend> inner_;
    int after_;
    std::atomic<int> calls_{0};
    mutable std::atomic<bool> inject_usage_pending_{false};
};
// Applies the TTS input hygiene above and the role->sid mapping in one place,
// recording what was actually sent so JSONL rows can show raw vs input text.
class TtsPipeline final : public ITtsBackend {
public:
    TtsPipeline(std::unique_ptr<ITtsBackend> inner, SidMap map)
        : inner_(std::move(inner)), map_(map) {}
    int sid_for(int speaker) const { return map_.for_speaker(speaker); }
    std::string last_raw() const { std::lock_guard lock(mu_); return last_raw_; }
    std::string last_input() const { std::lock_guard lock(mu_); return last_input_; }
    AudioBuffer synthesize(const std::string& text, int speaker) override {
        return do_synthesize(text, map_.for_speaker(speaker));
    }
    AudioBuffer synthesize_with_sid(const std::string& text, int sid) {
        return do_synthesize(text, sid);
    }
    std::string name() const override { return inner_->name(); }
private:
    AudioBuffer do_synthesize(const std::string& raw, int sid) {
        const std::string input = sanitize_tts_text(raw);
        if (input.empty()) throw std::invalid_argument("empty TTS text after sanitizing");
        {
            std::lock_guard lock(mu_);
            last_raw_ = raw;
            last_input_ = input;
        }
        return inner_->synthesize(input, sid + 1); // backends take 1-based speaker
    }
    std::unique_ptr<ITtsBackend> inner_;
    SidMap map_;
    mutable std::mutex mu_;
    std::string last_raw_, last_input_;
};
struct BankEntry {
    std::string case_id; // "*" = any
    std::string category; // "*" = any
    int speaker{};        // 0 = any
    std::vector<std::string> wavs;
    std::string text;
};
std::vector<BankEntry> load_bank(const std::string& manifest_path, const std::string& bank_dir) {
    std::ifstream f(manifest_path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open bank manifest: " + manifest_path);
    const fs::path base = bank_dir.empty() ? fs::path(manifest_path).parent_path() : fs::path(bank_dir);
    std::vector<BankEntry> entries;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto fields = split_fields(line, '\t');
        if (fields.size() != 5)
            throw std::runtime_error("bank manifest expects 5 columns on line " + std::to_string(lineno));
        BankEntry e;
        e.case_id = fields[0];
        e.category = fields[1];
        e.speaker = fields[2] == "*" ? 0 : parse_int(fields[2], "bank speaker");
        if (e.speaker < 0 || e.speaker > 3)
            throw std::runtime_error("bank speaker must be 1..3 or * on line " + std::to_string(lineno));
        e.wavs = split_fields(fields[3], ';');
        e.text = fields[4];
        if (e.text.empty()) throw std::runtime_error("bank text is empty on line " + std::to_string(lineno));
        for (const auto& wav : e.wavs) {
            if (wav.empty()) throw std::runtime_error("bank WAV path is empty on line " + std::to_string(lineno));
            const fs::path path = base / wav;
            if (!fs::exists(path))
                throw std::runtime_error("bank manifest references missing WAV: " + path.string());
        }
        for (const auto& previous : entries)
            if (previous.case_id == e.case_id && previous.category == e.category &&
                previous.speaker == e.speaker)
                throw std::runtime_error("duplicate bank key on line " + std::to_string(lineno));
        entries.push_back(std::move(e));
    }
    if (entries.empty()) throw std::runtime_error("bank manifest has no entries");
    return entries;
}
const BankEntry* find_bank(const std::vector<BankEntry>& entries, const std::string& case_id,
                           const std::string& category, int speaker) {
    const BankEntry* best = nullptr;
    int best_score = -1;
    for (const auto& e : entries) {
        if (e.case_id != "*" && e.case_id != case_id) continue;
        if (e.category != "*" && e.category != category) continue;
        if (e.speaker != 0 && e.speaker != speaker) continue;
        const int score = (e.case_id != "*" ? 4 : 0) + (e.category != "*" ? 2 : 0) + (e.speaker != 0 ? 1 : 0);
        if (score > best_score) { best_score = score; best = &e; }
    }
    return best;
}
struct RunContext {
    std::string run_id, mode, battery, dialogue_mode, approach, config;
    std::string llm_provider, tts_provider, observed_llm_name;
    std::string mnn_backend_requested, mnn_backend_effective;
    int mnn_threads{};
    std::string model_config_sha256;
    int deadline_ms{};
};
struct Work {
    const Case* c{};
    char strategy{'1'};
    int round{};
    int warmup_index{}; // 0 = formal round, >0 = warmup round number
    int session{1};
    std::string prompt;
    int attempt{1};
    bool first_try_violation{false};
};
struct EmitResult {
    TextAudit audit;
    std::string text;
    bool ok{};
};
struct RowMeta {
    std::string status;
    std::string text;
    std::string prompt;
    const SpeechResult* r{};
    std::string tts_raw, tts_input;
    bool cache_hit{};
    double bank_concat_ms{};
    std::string wav;
    double audio_s{};
    double rtf{};
    double handoff_ms{};
    int sample_rate{};
    bool formal{true};
    double e2e_pcm_ms{};
    bool e2e_valid{};
};
class DialogueRunner {
public:
    DialogueRunner(Opt& opt, RunContext& rc, std::ofstream& log, fs::path out,
                   const std::vector<Case>& cases, AiRuntime& runtime, TtsPipeline& tts,
                   const std::vector<BankEntry>& bank, fs::path bank_base)
        : opt_(opt), rc_(rc), log_(log), out_(std::move(out)), cases_(cases),
          runtime_(runtime), tts_(tts), bank_(bank), bank_base_(std::move(bank_base)) {}
    void run(std::vector<Work> work, const std::string& scenario, bool burst,
             std::size_t switch_idx, int deadline_ms, bool chain_history,
             bool assert_s5, bool assert_s6);
    std::size_t offered() const { return offered_; }
    std::size_t rejected() const { return rejected_; }
    std::size_t rows() const { return rows_; }
    std::size_t ok() const { return ok_; }
    std::size_t failed() const { return failed_; }
    std::size_t unresolved() const { return unresolved_; }
    int late_publish_violation() const { return late_publish_violation_; }
    int stale_audio_violation() const { return stale_audio_violation_; }
    std::vector<std::string> status_sequence() const { return status_sequence_; }
private:
    EmitResult emit(const Case& c, const Work& w, const RowMeta& meta);
    EmitResult deliver_bank(const Case& c, const Work& w, const BankEntry& entry);
    Opt& opt_;
    RunContext& rc_;
    std::ofstream& log_;
    fs::path out_;
    const std::vector<Case>& cases_;
    AiRuntime& runtime_;
    TtsPipeline& tts_;
    const std::vector<BankEntry>& bank_;
    fs::path bank_base_;
    std::string scenario_;
    std::size_t event_seq_{0};
    std::size_t offered_{0}, rejected_{0}, rows_{0}, ok_{0}, failed_{0}, unresolved_{0};
    int late_publish_violation_{0};
    int stale_audio_violation_{0};
    std::vector<std::string> status_sequence_;
};

EmitResult DialogueRunner::emit(const Case& c, const Work& w, const RowMeta& meta) {
    TextAudit audit = score_text(meta.text, meta.prompt, c.required_kw, c.forbidden_kw);
    // Contract definition: a row is usable for measurement only when the model
    // call succeeded and the provider is not the mock. Zero usage is not a test.
    const bool usage_valid = meta.status == "ok" && opt_.llm != "mock";
    const bool injected = meta.r && meta.r->error_message.find("injected llm fault") != std::string::npos;
    const bool truncated = meta.r && meta.r->llm_usage.truncated;
    const bool factual_ok = meta.status == "ok" && audit.required_missing == 0 &&
                            audit.violating_gate_hits == 0 && !truncated;
    log_ << "{\"type\":\"case\",\"schema\":" << kSchema
         << ",\"id\":" << json(c.id)
         << ",\"category\":" << json(c.category)
         << ",\"strategy\":\"P" << w.strategy << "\""
         << ",\"persona\":" << json(c.persona_key)
         << ",\"persona_key\":" << json(c.persona_key)
         << ",\"persona_traits\":" << json(persona_traits(c.persona_key))
         << ",\"round\":" << w.round
         << ",\"warmup\":" << jbool(w.warmup_index > 0)
         << ",\"attempt\":" << w.attempt
         << ",\"first_try_violation\":" << jbool(w.first_try_violation)
         << ",\"session_id\":" << (meta.r ? meta.r->session_id : w.session)
         << ",\"speaker\":" << c.speaker
         << ",\"mode\":" << json(rc_.mode)
         << ",\"battery\":" << json(rc_.battery)
         << ",\"scenario\":" << (scenario_.empty() ? "null" : json(scenario_))
         << ",\"dialogue_mode\":" << json(rc_.dialogue_mode)
         << ",\"approach\":" << json(rc_.approach)
         << ",\"config\":" << json(rc_.config)
         << ",\"deadline_ms\":" << rc_.deadline_ms
         << ",\"mnn_backend\":" << json(rc_.mnn_backend_requested)
         << ",\"mnn_backend_effective\":" << json(rc_.mnn_backend_effective)
         << ",\"mnn_threads\":" << rc_.mnn_threads
         << ",\"model_config_sha256\":" << json(rc_.model_config_sha256)
         << ",\"llm_provider\":" << json(opt_.llm)
         << ",\"tts_provider\":" << json(opt_.tts)
         << ",\"prompt_text\":" << json(meta.prompt)
         << ",\"fact\":" << json(c.fact)
         << ",\"history\":" << json(c.history)
         << ",\"structured\":" << json(c.structured)
         << ",\"expected_review_hint\":" << json(c.expected)
         << ",\"forbidden_review_hint\":" << json(c.forbidden)
         << ",\"required_kw\":" << json(c.required_kw)
         << ",\"forbidden_kw\":" << json(c.forbidden_kw)
         << ",\"status\":" << json(meta.status)
         << ",\"generated_text\":" << json(meta.text)
         << ",\"raw_llm_text\":" << json(meta.r ? meta.r->raw_llm_text : std::string())
         << ",\"error_message\":" << json(meta.r ? meta.r->error_message : std::string())
         << ",\"fallback_used\":" << jbool(meta.r && meta.r->fallback_used)
         << ",\"injected_fault\":" << jbool(injected)
         << ",\"truncated\":" << jbool(truncated)
         << ",\"status_code\":" << (meta.r ? meta.r->llm_usage.status_code : -1)
         << ",\"usage_valid\":" << jbool(usage_valid)
         << ",\"llm_called\":" << jbool(!meta.cache_hit)
         << ",\"cache_hit\":" << jbool(meta.cache_hit)
         << ",\"cache_miss\":" << jbool(rc_.dialogue_mode == "cache" && !meta.cache_hit)
         << ",\"bank_concat_ms\":" << (meta.cache_hit ? jnum(meta.bank_concat_ms) : std::string("null"))
         << ",\"text_codepoints\":" << audit.codepoints
         << ",\"prompt_overlap_codepoints\":" << audit.prompt_overlap_codepoints
         << ",\"factual_ok\":" << jbool(factual_ok)
         << ",\"score\":{"
         << "\"required_missing\":" << audit.required_missing
         << ",\"required_echo_suspect\":" << audit.required_echo_suspect
         << ",\"violating_gate_hits\":" << audit.violating_gate_hits
         << ",\"violating_prompt_echo_hits\":" << audit.violating_prompt_echo_hits
         << ",\"required\":[";
    for (std::size_t i = 0; i < audit.required.size(); ++i) {
        const auto& a = audit.required[i];
        if (i) log_ << ',';
        log_ << "{\"keyword\":" << json(a.keyword) << ",\"raw_hits\":" << a.raw_hits
             << ",\"negated_hits\":" << a.negated_hits
             << ",\"violating_hits\":" << a.violating_hits
             << ",\"in_prompt\":" << jbool(a.in_prompt)
             << ",\"echo_suspect\":" << jbool(a.echo_suspect) << ",\"contexts\":[";
        for (std::size_t k = 0; k < a.contexts.size(); ++k) {
            if (k) log_ << ',';
            log_ << json(a.contexts[k]);
        }
        log_ << "]}";
    }
    log_ << "],\"forbidden\":[";
    for (std::size_t i = 0; i < audit.forbidden.size(); ++i) {
        const auto& a = audit.forbidden[i];
        if (i) log_ << ',';
        log_ << "{\"keyword\":" << json(a.keyword) << ",\"raw_hits\":" << a.raw_hits
             << ",\"negated_hits\":" << a.negated_hits
             << ",\"violating_hits\":" << a.violating_hits
             << ",\"in_prompt\":" << jbool(a.in_prompt)
             << ",\"echo_suspect\":" << jbool(a.echo_suspect) << ",\"contexts\":[";
        for (std::size_t k = 0; k < a.contexts.size(); ++k) {
            if (k) log_ << ',';
            log_ << json(a.contexts[k]);
        }
        log_ << "]}";
    }
    log_ << "]}"
         << ",\"tts_raw_text\":" << json(meta.tts_raw)
         << ",\"tts_input_text\":" << json(meta.tts_input)
         << ",\"tts_sid\":" << tts_.sid_for(c.speaker)
         << ",\"audio_wav\":" << json(meta.wav)
         << ",\"sample_rate\":" << meta.sample_rate
         << ",\"audio_s\":" << jnum(meta.audio_s)
         << ",\"queue_llm_ms\":" << jnum(meta.r ? meta.r->queue_llm_ms : 0.0)
         << ",\"llm_ms\":" << jnum(meta.r ? meta.r->llm_ms : 0.0)
         << ",\"queue_tts_ms\":" << jnum(meta.r ? meta.r->queue_tts_ms : 0.0)
         << ",\"llm_ttfc_ms\":" << jnum(meta.r ? meta.r->llm_usage.ttfc_ms : 0.0)
         << ",\"prompt_tokens\":" << (meta.r ? meta.r->llm_usage.prompt_tokens : 0)
         << ",\"generated_tokens\":" << (meta.r ? meta.r->llm_usage.generated_tokens : 0)
         << ",\"prefill_ms\":" << jnum(meta.r ? meta.r->llm_usage.prefill_ms : 0.0)
         << ",\"decode_ms\":" << jnum(meta.r ? meta.r->llm_usage.decode_ms : 0.0)
         << ",\"tts_ms\":" << jnum(meta.r ? meta.r->tts_ms : 0.0)
         << ",\"tts_rtf\":" << jnum(meta.rtf)
         << ",\"end_to_end_pcm_ms\":" << (meta.e2e_valid ? jnum(meta.e2e_pcm_ms) : std::string("null"))
         << ",\"resolved_ms\":" << (meta.r ? jnum(meta.r->end_to_end_ms) : std::string("null"))
         << ",\"presentation_handoff_ms\":" << jnum(meta.handoff_ms)
         << ",\"first_audible_ms\":null"
         << ",\"first_audible_reason\":" << json(kFirstAudibleReason)
         << "}\n";
    ++rows_;
    status_sequence_.push_back(meta.status);
    if (meta.formal) {
        if (meta.status == "ok") ++ok_;
        else if (meta.status != "queue_rejected") ++failed_;
    }
    EmitResult result;
    result.audit = std::move(audit);
    result.text = meta.text;
    result.ok = meta.status == "ok";
    return result;
}
EmitResult DialogueRunner::deliver_bank(const Case& c, const Work& w, const BankEntry& entry) {
    // Cache mode C: no model, no TTS. Read the pre-rendered PCM, assert a single
    // sample rate, join clips with 120 ms of silence and hand the buffer over.
    const auto start = Clock::now();
    AudioBuffer audio;
    for (std::size_t i = 0; i < entry.wavs.size(); ++i) {
        const fs::path path = bank_base_ / entry.wavs[i];
        auto part = read_pcm16_wav(path.string());
        if (i == 0) audio.sample_rate = part.audio.sample_rate;
        else if (part.audio.sample_rate != audio.sample_rate)
            throw std::runtime_error("bank clips have mismatched sample rates: " + path.string());
        if (!audio.samples.empty()) {
            const std::size_t gap = static_cast<std::size_t>(audio.sample_rate) * 120u / 1000u;
            audio.samples.insert(audio.samples.end(), gap, 0.0f);
        }
        audio.samples.insert(audio.samples.end(), part.audio.samples.begin(), part.audio.samples.end());
    }
    const double concat_ms = milliseconds(Clock::now() - start);
    RowMeta meta;
    meta.status = "ok";
    meta.text = entry.text;
    meta.prompt = std::string();
    meta.cache_hit = true;
    meta.bank_concat_ms = concat_ms;
    meta.wav = c.id + "_P" + std::string(1, w.strategy) + "_a" + std::to_string(w.attempt) + "_bank.wav";
    try {
        write_pcm16_wav((out_ / meta.wav).string(), audio);
        auto readback = read_pcm16_wav((out_ / meta.wav).string());
        if (readback.audio.sample_rate != audio.sample_rate ||
            readback.audio.samples.size() != audio.samples.size())
            throw std::runtime_error("bank WAV read-back mismatch");
        meta.audio_s = static_cast<double>(audio.samples.size()) / audio.sample_rate;
        meta.sample_rate = audio.sample_rate;
    } catch (const std::exception&) {
        meta.status = "wav_error";
        meta.wav.clear();
        meta.sample_rate = 0;
    }
    meta.e2e_pcm_ms = milliseconds(Clock::now() - start);
    meta.e2e_valid = meta.status == "ok";
    auto result = emit(c, w, meta);
    std::cout << "[bank] " << c.id << " P" << w.strategy << ' ' << meta.status
              << " concat_ms=" << concat_ms << " audio_s=" << meta.audio_s << '\n';
    return result;
}
void DialogueRunner::run(std::vector<Work> work, const std::string& scenario, bool burst,
                         std::size_t switch_idx, int deadline_ms, bool chain_history,
                         bool assert_s5, bool assert_s6) {
    (void)assert_s5;
    (void)assert_s6;
    scenario_ = scenario;
    std::map<EventId, Work*> by_event;
    std::set<EventId> pre_flip;
    bool flip_done = false;
    std::string last_text;
    auto append_history = [&](Work& w) {
        // Chained scenarios pass the previous spoken line as explicit history;
        // it is fixture context, not hidden state.
        if (chain_history && !last_text.empty())
            w.prompt += " 上一句台词是『" + last_text + "』；请承接最新事实但不要重复。";
    };
    auto submit = [&](Work& w) -> bool {
        DialogueRequest req{};
        req.session_id = w.session;
        req.event_id = static_cast<EventId>(++event_seq_);
        req.speaker = w.c->speaker;
        req.persona = w.c->persona_key;
        req.prompt = w.prompt;
        req.fallback_line = "（暂时无语音）";
        req.created = Clock::now();
        req.deadline = req.created + std::chrono::milliseconds(deadline_ms);
        const EventId eid = req.event_id;
        by_event[eid] = &w;
        if (!flip_done) pre_flip.insert(eid);
        const bool accepted = runtime_.submit(std::move(req));
        if (accepted) {
            ++offered_;
        } else {
            ++rejected_;
            by_event.erase(eid);
            RowMeta meta;
            meta.status = "queue_rejected";
            meta.prompt = w.prompt;
            meta.formal = w.warmup_index == 0;
            emit(*w.c, w, meta);
        }
        return accepted;
    };
    auto drain = [&](std::optional<SpeechResult> result) -> std::optional<EmitResult> {
        if (!result) return std::nullopt;
        auto it = by_event.find(result->event_id);
        if (it == by_event.end()) return std::nullopt;
        Work& w = *it->second;
        by_event.erase(it);
        const Case& c = *w.c;
        RowMeta meta;
        meta.status = result->status;
        meta.text = result->text;
        meta.prompt = w.prompt;
        meta.r = &*result;
        meta.formal = w.warmup_index == 0;
        if (result->status == "ok") {
            // The TTS stage received exactly this text; sanitizing is deterministic.
            meta.tts_raw = result->text;
            meta.tts_input = sanitize_tts_text(result->text);
        }
        if (result->status == "ok" && !result->audio.samples.empty() && result->audio.sample_rate > 0) {
            // Warmups must not clobber formal WAVs; keep round and attempt in the name.
            std::string suffix = w.warmup_index > 0
                ? "_w" + std::to_string(w.warmup_index)
                : "_r" + std::to_string(w.round);
            if (w.attempt > 1) suffix += "_a" + std::to_string(w.attempt);
            meta.wav = c.id + "_P" + std::string(1, w.strategy) + suffix + ".wav";
            try {
                write_pcm16_wav((out_ / meta.wav).string(), result->audio);
                // Read back immediately: a truncated or mis-headed WAV must not be
                // counted as an ok row.
                auto readback = read_pcm16_wav((out_ / meta.wav).string());
                if (readback.audio.sample_rate != result->audio.sample_rate ||
                    readback.audio.samples.size() != result->audio.samples.size())
                    throw std::runtime_error("WAV read-back mismatch");
                meta.audio_s = static_cast<double>(result->audio.samples.size()) / result->audio.sample_rate;
                meta.rtf = (result->tts_ms / 1000.0) / meta.audio_s;
                meta.sample_rate = result->audio.sample_rate;
            } catch (const std::exception&) {
                meta.wav.clear();
                meta.status = "wav_error";
                meta.sample_rate = 0;
            }
        }
        meta.handoff_ms = milliseconds(Clock::now() - result->pcm_completed_at);
        meta.e2e_pcm_ms = result->end_to_end_ms;
        meta.e2e_valid = meta.status == "ok";
        if (flip_done && pre_flip.count(result->event_id)) {
            // The probe knows the flip instant: an ok result for a pre-flip event
            // is a late publish, and a stale result that still carries audio is a
            // second violation. Both are counted, not assumed impossible.
            if (meta.status == "ok") ++late_publish_violation_;
            if (meta.status == "stale" && !result->audio.samples.empty()) ++stale_audio_violation_;
        }
        auto emitted = emit(c, w, meta);
        if (meta.status == "ok" && !result->text.empty()) last_text = result->text;
        std::cout << '[' << rows_ << ']' << (scenario_.empty() ? "" : " " + scenario_) << ' '
                  << c.id << " P" << w.strategy
                  << (w.warmup_index > 0 ? "(w)" : "") << " r" << w.round
                  << " a" << w.attempt << ' ' << meta.status
                  << (meta.r && meta.r->error_message.find("injected llm fault") != std::string::npos
                          ? " injected_fault=true" : "")
                  << ' '
                  << (meta.status == "ok" ? meta.text : std::string("(no audio)")) << '\n';
        return emitted;
    };
    auto await = [&]() -> std::optional<SpeechResult> {
        std::optional<SpeechResult> result;
        const auto until = Clock::now() + std::chrono::milliseconds(deadline_ms + 3000);
        while (Clock::now() < until) {
            if ((result = runtime_.poll())) break;
            std::this_thread::sleep_for(1ms);
        }
        return result;
    };
    auto deliver_cache = [&](Work& w) -> bool {
        if (opt_.dialogue_mode != "cache") return false;
        const BankEntry* entry = find_bank(bank_, w.c->id, w.c->category, w.c->speaker);
        if (!entry) return false; // miss falls back to free generation, recorded per row
        deliver_bank(*w.c, w, *entry);
        last_text = entry->text;
        return true;
    };
    if (burst) {
        // Fire every request without waiting so the bounded queue and deadline
        // machinery decide rejection/expiry instead of the harness.
        for (std::size_t i = 0; i < work.size(); ++i) {
            Work& w = work[i];
            append_history(w);
            w.session = i < switch_idx ? 1 : 2;
            if (deliver_cache(w)) continue;
            submit(w);
            if (switch_idx < work.size() && i + 1 == switch_idx) {
                runtime_.set_active_session(2);
                flip_done = true;
                std::cout << "[flip] active session -> 2 after " << switch_idx << " submissions\n";
            }
        }
        const auto until = Clock::now() + std::chrono::milliseconds(deadline_ms + 180000);
        while (!by_event.empty() && Clock::now() < until) {
            if (auto r = runtime_.poll()) drain(std::move(r));
            else std::this_thread::sleep_for(1ms);
        }
        unresolved_ = by_event.size();
        return;
    }
    for (std::size_t i = 0; i < work.size(); ++i) {
        Work& w = work[i];
        append_history(w);
        w.session = i < switch_idx ? 1 : 2;
        if (deliver_cache(w)) continue;
        if (!submit(w)) continue;
        auto emitted = drain(await());
        if (opt_.dialogue_mode == "template" && w.attempt == 1 && emitted && emitted->ok) {
            // Mode B: machine check after generation; one constrained re-prompt
            // that may only reword. The first attempt's violation stays on record.
            std::vector<std::string> reasons;
            const auto& audit = emitted->audit;
            if (audit.codepoints < 6 || audit.codepoints > 20) reasons.push_back("字数不在6到20");
            if (audit.required_missing > 0) reasons.push_back("缺少要求的关键词");
            if (audit.violating_gate_hits > 0) reasons.push_back("出现禁止词或臆测");
            if (!reasons.empty()) {
                Work retry = w;
                retry.attempt = 2;
                retry.first_try_violation = true;
                retry.prompt = strict_retry_prompt(*w.c, reasons);
                submit(retry);
                drain(await());
            }
        }
    }
    unresolved_ = by_event.size();
}
} // namespace

int main(int argc, char** argv) {
    try {
        auto opt = parse(argc, argv);
        fs::path out = fs::path(opt.output) / opt.run_id;
        fs::create_directories(out);
        const fs::path results_path = out / "results.jsonl";
        if (fs::exists(results_path) && !opt.force)
            throw std::runtime_error("results.jsonl already exists (use --force to overwrite): " +
                                     results_path.string());
        std::ofstream log(results_path, std::ios::binary | std::ios::trunc);
        if (!log) throw std::runtime_error("cannot open results.jsonl");
        log << std::fixed << std::setprecision(3);

        const SidMap sids = parse_sids(opt.tts_sids);
        std::vector<Case> cases;
        std::vector<TtsLine> tts_lines;
        if (opt.battery != "tts_lines") cases = load_cases(opt.cases);
        if (opt.battery == "tts_lines") tts_lines = load_tts_lines(opt.tts_lines);

        // Model evidence: config hash is what the per-case rows cite, so it must
        // be computed before any inference. The config is small; weights are
        // hashed once by the provenance runner, not per probe invocation.
        std::string model_config_sha256;
        std::string model_sampling;
        std::string model_dir;
        std::string model_file;
        std::string config_backend, config_threads;
        double cfg_temperature = 0, cfg_top_p = 0, cfg_top_k = 0, cfg_rep = 0;
        if (opt.llm == "mnn") {
            const std::string config_text = read_text_file(opt.mnn_config);
            model_config_sha256 = aimj_probe::Sha256::file_hex(opt.mnn_config);
            if (model_config_sha256.empty())
                throw std::runtime_error("cannot read --mnn-config: " + opt.mnn_config);
            model_dir = fs::path(opt.mnn_config).parent_path().string();
            model_file = json_string_field(config_text, "llm_model");
            config_backend = json_string_field(config_text, "backend_type");
            const double threads = json_number_field(config_text, "thread_num", 0);
            config_threads = threads > 0 ? std::to_string(static_cast<int>(threads)) : "";
            cfg_temperature = json_number_field(config_text, "temperature", 0);
            cfg_top_p = json_number_field(config_text, "top_p", 0);
            cfg_top_k = json_number_field(config_text, "top_k", 0);
            cfg_rep = json_number_field(config_text, "repetition_penalty", 0);
            std::ostringstream sampling;
            sampling << "{\"max_new_tokens\":" << opt.max_tokens
                     << ",\"temperature\":" << jnum(cfg_temperature)
                     << ",\"top_p\":" << jnum(cfg_top_p)
                     << ",\"top_k\":" << jnum(cfg_top_k)
                     << ",\"repetition_penalty\":" << jnum(cfg_rep) << '}';
            model_sampling = sampling.str();
        }

        // Providers ---------------------------------------------------------
        std::unique_ptr<ILlmBackend> llm;
        auto init_start = Clock::now();
        auto make_llm = [&](const std::string& backend) -> std::unique_ptr<ILlmBackend> {
            if (opt.llm == "mock") return std::make_unique<MockLlm>(10ms);
#ifdef AIMJ_WITH_MNN
            if (opt.llm == "mnn")
                return make_mnn_backend(opt.mnn_config, opt.max_tokens, backend, opt.mnn_threads);
#endif
            throw std::invalid_argument("LLM provider unavailable: " + opt.llm);
        };
        std::string backend_effective = "not_applicable";
        RunContext rc;
        rc.mnn_backend_requested = opt.mnn_backend.empty() ? "config_default" : opt.mnn_backend;
        rc.mnn_threads = opt.mnn_threads;
        rc.model_config_sha256 = model_config_sha256;
        rc.deadline_ms = opt.deadline_ms;
        rc.llm_provider = opt.llm;
        rc.tts_provider = opt.tts;
        rc.run_id = opt.run_id;
        rc.battery = opt.battery;
        rc.dialogue_mode = opt.dialogue_mode;
        rc.approach = opt.approach;
        rc.mode = (opt.llm == "mnn" && opt.tts == "sherpa") ? "real"
                  : (opt.llm == "mock" && opt.tts == "mock") ? "mock"
                                                            : "mixed";
        if (opt.config_name.empty()) {
            rc.config = opt.llm == "mock" ? "mock"
                        : opt.mnn_backend == "opencl" ? "opencl"
                        : opt.mnn_threads > 0 ? "cpu" + std::to_string(opt.mnn_threads)
                                              : "cpu_default";
        } else {
            rc.config = opt.config_name;
        }
#ifdef AIMJ_WITH_MNN
        const OpenClDiagnostics opencl = opencl_diagnostics();
#else
        struct { bool mnn_dll_loaded{}; std::string mnn_dll_path; bool mnn_opencl_symbols{};
                 bool icd_available{}; std::string icd_detail; } opencl;
#endif
        try {
            llm = make_llm(opt.mnn_backend);
            if (opt.mnn_backend == "opencl") backend_effective = "opencl_verified";
            else if (opt.llm == "mnn") backend_effective = "cpu_dll";
        } catch (const std::exception& e) {
            const std::string message = e.what();
            if (opt.mnn_backend == "opencl" && message.find("backend_unavailable") != std::string::npos) {
                backend_effective = "backend_unavailable";
                if (!opt.allow_backend_fallback) throw;
                std::cerr << "WARNING: " << message << "; --allow-backend-fallback given, using CPU\n";
                llm = make_llm("cpu");
                backend_effective = "cpu_fallback";
            } else {
                throw;
            }
        }
        rc.mnn_backend_effective = backend_effective;
        if (opt.inject_after >= 0) llm = std::make_unique<FaultInjectLlm>(std::move(llm), opt.inject_after);

        std::unique_ptr<ITtsBackend> tts;
        if (opt.tts == "mock") tts = std::make_unique<MockTts>(10ms);
#ifdef AIMJ_WITH_SHERPA
        else if (opt.tts == "sherpa")
            tts = make_sherpa_vits_backend(opt.tts_model, opt.tts_tokens,
                                           opt.tts_lexicon, opt.tts_threads, opt.tts_rule_fars);
#endif
        else throw std::invalid_argument("TTS provider unavailable: " + opt.tts);
        auto tts_pipeline = std::make_unique<TtsPipeline>(std::move(tts), sids);
        TtsPipeline& tts_ref = *tts_pipeline;
        double init_ms = milliseconds(Clock::now() - init_start);
        // Plain-text proof line for console.out; the same facts are in the run
        // header. A CPU request never reaches this branch.
        if (opt.mnn_backend == "opencl" && backend_effective == "opencl_verified") {
            std::cout << "[opencl] backend_effective=opencl_verified dll=" << opencl.mnn_dll_path
                      << " symbols=" << (opencl.mnn_opencl_symbols ? "present" : "missing")
                      << " icd=" << opencl.icd_detail << '\n';
        }

        // Bank manifest is validated before anything runs: a missing WAV means
        // the cache arm cannot be reproduced and must fail fast, not silently miss.
        std::vector<BankEntry> bank;
        if (opt.dialogue_mode == "cache") bank = load_bank(opt.bank_manifest, opt.bank_dir);

        // Run header -------------------------------------------------------
        const std::string cases_bytes = std::to_string(fs::exists(opt.cases) ? fs::file_size(opt.cases) : 0);
        const std::string cases_sha = aimj_probe::Sha256::file_hex(opt.cases);
        std::string tts_bytes = "0", tts_sha;
        if (!opt.tts_lines.empty()) {
            tts_bytes = std::to_string(fs::exists(opt.tts_lines) ? fs::file_size(opt.tts_lines) : 0);
            tts_sha = aimj_probe::Sha256::file_hex(opt.tts_lines);
        }
        const std::string mnn_dll_version = file_version(opencl.mnn_dll_path);
        const std::string mnn_dll_sha = opencl.mnn_dll_path.empty()
            ? std::string() : aimj_probe::Sha256::file_hex(opencl.mnn_dll_path);
        const std::string mnn_version_resolved =
            !mnn_dll_version.empty() ? mnn_dll_version
            : !version_from_path(opencl.mnn_dll_path).empty() ? version_from_path(opencl.mnn_dll_path)
            : opt.llm_sdk_label;
        const std::string mnn_version_source =
            !mnn_dll_version.empty() ? "dll_version_resource"
            : !version_from_path(opencl.mnn_dll_path).empty() ? "path"
            : !opt.llm_sdk_label.empty() ? "cli_label" : "unavailable";
        std::string tts_dll_version, tts_dll_path;
#if defined(_WIN32)
        tts_dll_path = loaded_module_file(L"sherpa-onnx-c-api.dll");
        if (tts_dll_path.empty()) tts_dll_path = loaded_module_file(L"sherpa-onnx.dll");
        tts_dll_version = file_version(tts_dll_path);
#endif
        const std::string tts_version_resolved =
            !tts_dll_version.empty() ? tts_dll_version
            : !version_from_path(tts_dll_path).empty() ? version_from_path(tts_dll_path)
            : opt.tts_sdk_label;
        const std::string tts_version_source =
            !tts_dll_version.empty() ? "dll_version_resource"
            : !version_from_path(tts_dll_path).empty() ? "path"
            : !opt.tts_sdk_label.empty() ? "cli_label" : "unavailable";
        const std::string tts_dll_sha = tts_dll_path.empty()
            ? std::string() : aimj_probe::Sha256::file_hex(tts_dll_path);
        std::string opencl_cache_file;
        bool opencl_cache_present = false;
        {
            // MNN writes <tmpPath>/mnn_cachefile.bin with tmpPath defaulting to
            // ".", and flushes it when the executor is destroyed at process exit.
            // This header field is therefore explicitly "at start"; the runner
            // records post-exit presence as the authoritative per-run evidence.
            std::array<fs::path, 5> candidates{
                fs::path("mnn_cachefile.bin"),
                fs::path("tmp") / "mnn_cachefile.bin",
                fs::path(model_dir) / "mnn_cachefile.bin",
                fs::path(model_dir) / "tmp" / "mnn_cachefile.bin",
                fs::path("..") / "mnn_cachefile.bin"};
            for (const auto& candidate : candidates)
                if (fs::exists(candidate)) { opencl_cache_file = candidate.string(); opencl_cache_present = true; }
        }
        log << "{\"type\":\"run\",\"schema\":" << kSchema
            << ",\"run_id\":" << json(opt.run_id)
            << ",\"mode\":" << json(rc.mode)
            << ",\"battery\":" << json(opt.battery)
            << ",\"dialogue_mode\":" << json(opt.dialogue_mode)
            << ",\"approach\":" << json(opt.approach)
            << ",\"config\":" << json(rc.config)
            << ",\"llm_provider\":" << json(opt.llm)
            << ",\"tts_provider\":" << json(opt.tts)
            << ",\"observed_llm_name\":" << json(llm->name())
            << ",\"mnn_backend_requested\":" << json(rc.mnn_backend_requested)
            << ",\"mnn_backend_effective\":" << json(rc.mnn_backend_effective)
            << ",\"mnn_threads\":" << rc.mnn_threads
            << ",\"mnn_config\":" << json(opt.mnn_config)
            << ",\"mnn_config_bytes\":" << (opt.mnn_config.empty() ? 0ull : fs::file_size(opt.mnn_config))
            << ",\"model_config_sha256\":" << json(model_config_sha256)
            << ",\"model_dir\":" << json(model_dir)
            << ",\"model_file\":" << json(model_file)
            << ",\"max_new_tokens\":" << opt.max_tokens
            << ",\"sampling\":" << (model_sampling.empty() ? std::string("null") : model_sampling)
            << ",\"config_backend\":" << json(config_backend)
            << ",\"config_threads\":" << json(config_threads)
            << ",\"llm_sdk\":\"mnn\",\"llm_sdk_version\":" << json(mnn_version_resolved)
            << ",\"llm_sdk_version_source\":" << json(mnn_version_source)
            << ",\"mnn_dll_sha256\":" << json(mnn_dll_sha)
            << ",\"tts_sdk\":\"sherpa-onnx\",\"tts_sdk_version\":" << json(tts_version_resolved)
            << ",\"tts_sdk_version_source\":" << json(tts_version_source)
            << ",\"tts_dll_path\":" << json(tts_dll_path)
            << ",\"tts_dll_sha256\":" << json(tts_dll_sha)
            << ",\"mnn_dll_path\":" << json(opencl.mnn_dll_path)
            << ",\"mnn_dll_opencl_symbols\":" << jbool(opencl.mnn_opencl_symbols)
            << ",\"opencl_icd_available\":" << jbool(opencl.icd_available)
            << ",\"opencl_icd_detail\":" << json(opencl.icd_detail)
            << ",\"opencl_cache_file\":" << json(opencl_cache_file)
            << ",\"opencl_cache_file_present_at_start\":" << jbool(opencl_cache_present)
            << ",\"cases_path\":" << json(opt.cases)
            << ",\"cases_bytes\":" << cases_bytes
            << ",\"cases_sha256\":" << json(cases_sha)
            << ",\"cases\":" << cases.size()
            << ",\"tts_lines_path\":" << json(opt.tts_lines)
            << ",\"tts_lines_bytes\":" << tts_bytes
            << ",\"tts_lines_sha256\":" << json(tts_sha)
            << ",\"tts_lines\":" << tts_lines.size()
            << ",\"strategies\":" << json(opt.strategies)
            << ",\"strategy_templates\":{"
            << "\"P0\":" << json(kP0Template) << ",\"P0_sha256\":"
            << json(aimj_probe::Sha256::hex_digest(kP0Template))
            << ",\"P1\":" << json("verbatim TSV prompt column per case")
            << ",\"P2\":" << json(kP2Template) << ",\"P2_sha256\":"
            << json(aimj_probe::Sha256::hex_digest(kP2Template))
            << ",\"P3\":" << json(kP3Template) << ",\"P3_sha256\":"
            << json(aimj_probe::Sha256::hex_digest(kP3Template))
            << ",\"TEMPLATE\":" << json(kTemplateModeTemplate) << ",\"TEMPLATE_sha256\":"
            << json(aimj_probe::Sha256::hex_digest(kTemplateModeTemplate)) << '}'
            << ",\"rounds\":" << opt.rounds
            << ",\"warmup_rounds\":" << opt.warmup_rounds
            << ",\"queue\":" << opt.queue
            << ",\"deadline_ms\":" << opt.deadline_ms
            << ",\"burst\":" << jbool(opt.burst)
            << ",\"switch_session_after\":" << opt.switch_session_after
            << ",\"inject_llm_error_after\":" << opt.inject_after
            << ",\"tts_sids\":[" << sids.sid[0] << ',' << sids.sid[1] << ',' << sids.sid[2] << ']'
            << ",\"chain_history\":" << jbool(opt.chain_history)
            << ",\"provider_init_ms\":" << jnum(init_ms)
            << ",\"first_audible_ms\":null"
            << ",\"first_audible_reason\":" << json(kFirstAudibleReason)
            << "}\n";

        // ---- TTS-only battery -------------------------------------------
        if (opt.battery == "tts_lines") {
            std::size_t ok = 0, failed = 0;
            for (std::size_t i = 0; i < tts_lines.size(); ++i) {
                const auto& l = tts_lines[i];
                const int sid = l.sid >= 0 ? l.sid : sids.for_speaker(l.speaker);
                auto start = Clock::now();
                std::string status = "ok";
                AudioBuffer audio;
                std::string raw = l.text, input;
                try {
                    audio = tts_ref.synthesize_with_sid(l.text, sid);
                    input = tts_ref.last_input();
                    raw = tts_ref.last_raw();
                    if (audio.samples.empty() || audio.sample_rate <= 0)
                        throw std::runtime_error("empty audio");
                } catch (const std::exception&) {
                    status = "error";
                }
                double synth_ms = milliseconds(Clock::now() - start);
                std::string wav;
                double audio_s = 0;
                std::string verify = "not_written";
                if (status == "ok") {
                    wav = l.id + ".wav";
                    try {
                        const auto written = write_pcm16_wav((out / wav).string(), audio);
                        auto readback = read_pcm16_wav((out / wav).string());
                        if (readback.audio.sample_rate != audio.sample_rate ||
                            readback.audio.samples.size() != audio.samples.size())
                            throw std::runtime_error("read-back mismatch");
                        verify = written.clipped_samples > 0 ? "ok_with_clipping" : "ok";
                        audio_s = static_cast<double>(audio.samples.size()) / audio.sample_rate;
                        ++ok;
                    } catch (const std::exception& e) {
                        status = "wav_verify_error";
                        verify = e.what();
                        wav.clear();
                        ++failed;
                    }
                } else {
                    ++failed;
                }
                log << "{\"type\":\"tts_line\",\"schema\":" << kSchema
                    << ",\"id\":" << json(l.id)
                    << ",\"category\":" << json(l.category)
                    << ",\"speaker\":" << l.speaker
                    << ",\"sid\":" << sid
                    << ",\"raw_text\":" << json(raw)
                    << ",\"tts_input_text\":" << json(input)
                    << ",\"status\":" << json(status)
                    << ",\"wav_verify\":" << json(verify)
                    << ",\"wav\":" << json(wav)
                    << ",\"sample_rate\":" << (status == "ok" ? audio.sample_rate : 0)
                    << ",\"pcm_samples\":" << (status == "ok" ? audio.samples.size() : 0)
                    << ",\"audio_s\":" << jnum(audio_s)
                    << ",\"synth_ms\":" << jnum(synth_ms)
                    << ",\"rtf\":" << (audio_s > 0 ? jnum((synth_ms / 1000.0) / audio_s) : std::string("null"))
                    << ",\"first_audible_ms\":null"
                    << ",\"first_audible_reason\":" << json(kFirstAudibleReason)
                    << "}\n";
                std::cout << '[' << (i + 1) << '/' << tts_lines.size() << "] " << l.id
                          << " sid=" << sid << ' ' << status << " synth_ms=" << synth_ms
                          << " audio_s=" << audio_s << '\n';
            }
            log << "{\"type\":\"summary\",\"schema\":" << kSchema
                << ",\"run_id\":" << json(opt.run_id)
                << ",\"battery\":\"tts_lines\",\"mode\":" << json(rc.mode)
                << ",\"lines\":" << tts_lines.size()
                << ",\"ok_with_wav\":" << ok << ",\"failed\":" << failed
                << ",\"peak_rss_bytes\":" << peak_rss_bytes()
                << ",\"quality_human_graded\":false,\"first_audible_measured\":false}\n";
            log.flush();
            std::cout << "Results: " << results_path.string()
                      << "; WAV=" << ok << "; failures=" << failed << '\n';
            return failed == 0 ? 0 : 2;
        }

        AiRuntime runtime(std::move(llm), std::move(tts_pipeline), static_cast<std::size_t>(opt.queue));
        const fs::path bank_base = opt.bank_dir.empty()
            ? fs::path(opt.bank_manifest).parent_path()
            : fs::path(opt.bank_dir);
        DialogueRunner runner(opt, rc, log, out, cases, runtime, tts_ref, bank, bank_base);

        // ---- Work list ---------------------------------------------------
        std::vector<char> strategies;
        for (char c : opt.strategies)
            if (c == '0' || c == '1' || c == '2' || c == '3') strategies.push_back(c);
        if (strategies.empty()) throw std::invalid_argument("no valid strategy in --strategies");
        if (opt.dialogue_mode != "free" && strategies.size() > 1)
            throw std::invalid_argument("template/cache modes run exactly one strategy");

        auto make_work = [&](const Case& c, char strategy, int round, int warmup_index) {
            Work w;
            w.c = &c;
            w.strategy = strategy;
            w.round = round;
            w.warmup_index = warmup_index;
            w.session = 1;
            w.prompt = opt.dialogue_mode == "template" ? template_prompt(c) : strategy_prompt(c, strategy);
            return w;
        };
        auto pick = [&](const std::string& id) -> const Case* {
            for (const auto& c : cases)
                if (c.id == id) return &c;
            return nullptr;
        };
        auto pick_category_speaker = [&](const std::string& category, int speaker) -> const Case* {
            for (const auto& c : cases)
                if (c.category == category && c.speaker == speaker) return &c;
            return nullptr;
        };

        if (opt.battery == "scenarios") {
            const std::string s = opt.scenario;
            std::vector<Work> work;
            bool burst = false, chain = false, assert_s5 = false, assert_s6 = false;
            std::size_t switch_idx = std::numeric_limits<std::size_t>::max();
            int deadline = opt.deadline_ms;
            if (s == "s1") {
                const Case* c = pick("event_discard_9tong");
                if (!c) c = pick_category_speaker("event", 1);
                if (!c) throw std::runtime_error("s1 needs an event fixture for speaker 1");
                work.push_back(make_work(*c, '1', 1, 0));
            } else if (s == "s2") {
                const Case* a = pick("event_pong");
                const Case* b = pick("event_pong_react");
                if (!a || !b) throw std::runtime_error("s2 needs event_pong and event_pong_react fixtures");
                work.push_back(make_work(*a, '1', 1, 0));
                work.push_back(make_work(*b, '1', 1, 0));
                chain = true;
            } else if (s == "s3") {
                std::vector<const Case*> picked;
                for (int speaker = 1; speaker <= 3; ++speaker) {
                    if (const Case* c = pick_category_speaker("event", speaker)) picked.push_back(c);
                }
                if (picked.size() != 3) throw std::runtime_error("s3 needs event fixtures for speakers 1..3");
                for (const Case* c : picked) work.push_back(make_work(*c, '1', 1, 0));
                chain = true;
            } else if (s == "s4") {
                // Rapid taps: submit everything at once with a tight deadline and
                // record how the bounded queue splits rejection from expiry.
                burst = true;
                deadline = std::max(1, std::min(opt.deadline_ms, 5));
                for (const auto& c : cases) work.push_back(make_work(c, '1', 1, 0));
            } else if (s == "s5") {
                // Session switch: queue 8 + burst + flip after 2 submissions. All
                // post-flip results must be stale with no audio; any ok result for
                // a pre-flip event is counted as a late publish.
                burst = true;
                assert_s5 = true;
                switch_idx = 2;
                if (opt.queue < 8)
                    throw std::runtime_error("s5 requires --queue 8 (got " + std::to_string(opt.queue) + ")");
                std::size_t taken = 0;
                for (const auto& c : cases) {
                    if (taken == 6) break;
                    work.push_back(make_work(c, '1', 1, 0));
                    ++taken;
                }
                if (work.size() != 6) throw std::runtime_error("s5 needs 6 fixtures");
            } else if (s == "s6") {
                // One model failure must not poison the next event.
                assert_s6 = true;
                if (opt.inject_after < 0)
                    throw std::runtime_error("s6 requires --inject-llm-error-after 0");
                std::size_t taken = 0;
                for (const auto& c : cases) {
                    if (taken == 2) break;
                    work.push_back(make_work(c, '1', 1, 0));
                    ++taken;
                }
                if (work.size() != 2) throw std::runtime_error("s6 needs 2 fixtures");
            } else {
                throw std::invalid_argument("unknown scenario: " + s);
            }
            runner.run(std::move(work), s, burst, switch_idx, deadline, chain, assert_s5, assert_s6);
            const auto stats = runtime.counters();
            log << "{\"type\":\"scenario\",\"schema\":" << kSchema
                << ",\"scenario\":" << json(s)
                << ",\"run_id\":" << json(opt.run_id)
                << ",\"rows\":" << runner.rows() << ",\"ok\":" << runner.ok()
                << ",\"failed\":" << runner.failed()
                << ",\"offered\":" << runner.offered() << ",\"submit_rejected\":" << runner.rejected()
                << ",\"unresolved\":" << runner.unresolved()
                << ",\"late_publish_violation\":" << runner.late_publish_violation()
                << ",\"stale_audio_violation\":" << runner.stale_audio_violation()
                << ",\"runtime_expired\":" << stats.expired
                << ",\"runtime_stale\":" << stats.stale
                << ",\"runtime_failed\":" << stats.failed
                << ",\"runtime_result_overflow\":" << stats.result_overflow
                << ",\"peak_rss_bytes\":" << peak_rss_bytes()
                << ",\"first_audible_measured\":false}\n";
            log.flush();
            if (assert_s5 && runner.rejected() != 0)
                throw std::runtime_error("s5 configuration error: queue_rejected=" +
                                         std::to_string(runner.rejected()) + " (expected 0)");
            if (assert_s6) {
                const auto seq = runner.status_sequence();
                if (seq.size() != 2 || seq[0] != "error" || seq[1] != "ok")
                    throw std::runtime_error("s6 assertion failed: expected error then ok");
            }
            std::cout << "Results: " << results_path.string()
                      << "; rows=" << runner.rows() << "; ok=" << runner.ok()
                      << "; failures=" << runner.failed() << '\n';
            return runner.failed() == 0 ? 0 : 2;
        }

        std::vector<Work> work;
        for (int w = 0; w < opt.warmup_rounds; ++w)
            for (char s : strategies)
                for (const auto& c : cases) work.push_back(make_work(c, s, 0, w + 1));
        for (int r = 1; r <= opt.rounds; ++r)
            for (char s : strategies)
                for (const auto& c : cases) work.push_back(make_work(c, s, r, 0));
        const std::size_t submitted_total = work.size();
        const std::size_t switch_idx = opt.switch_session_after >= 0
            ? static_cast<std::size_t>(opt.switch_session_after) : work.size();
        runner.run(std::move(work), "", opt.burst, switch_idx, opt.deadline_ms,
                   opt.chain_history, false, false);
        const auto stats = runtime.counters();
        log << "{\"type\":\"summary\",\"schema\":" << kSchema
            << ",\"run_id\":" << json(opt.run_id)
            << ",\"battery\":" << json(opt.battery)
            << ",\"mode\":" << json(rc.mode)
            << ",\"config\":" << json(rc.config)
            << ",\"work_items\":" << submitted_total
            << ",\"offered\":" << runner.offered() << ",\"submit_rejected\":" << runner.rejected()
            << ",\"unresolved\":" << runner.unresolved()
            << ",\"resolved\":" << runner.rows()
            << ",\"ok_with_wav\":" << runner.ok()
            << ",\"failed_or_missing\":" << runner.failed()
            << ",\"runtime_accepted\":" << stats.accepted
            << ",\"runtime_queue_rejected\":" << stats.queue_rejected
            << ",\"runtime_expired\":" << stats.expired
            << ",\"runtime_stale\":" << stats.stale
            << ",\"runtime_failed\":" << stats.failed
            << ",\"result_overflow\":" << stats.result_overflow
            << ",\"late_publish_violation\":" << runner.late_publish_violation()
            << ",\"stale_audio_violation\":" << runner.stale_audio_violation()
            << ",\"peak_rss_bytes\":" << peak_rss_bytes()
            << ",\"quality_human_graded\":false,\"first_audible_measured\":false}\n";
        log.flush();
        std::cout << "Results: " << results_path.string()
                  << "; offered=" << runner.offered() << " submit_rejected=" << runner.rejected()
                  << "; WAV=" << runner.ok() << "; failures=" << runner.failed()
                  << "; peak_rss_bytes=" << peak_rss_bytes() << '\n';
        return runner.failed() == 0 ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
