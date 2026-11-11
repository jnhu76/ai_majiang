#include "aimj/audio_player.hpp"
#include "aimj/voice_loop.hpp"
#include "aimj/wav.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#ifdef AIMJ_WITH_MNN
#include "aimj/runtime.hpp"
#endif

namespace fs = std::filesystem;
using aimj::AudioBuffer;
using aimj::Clock;
using aimj::voice::Bank;
using aimj::voice::BankEntry;
using aimj::voice::Director;
using aimj::voice::PublicEvent;
using aimj::voice::Selection;

namespace {

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string json(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                                   << static_cast<int>(ch) << std::dec;
                else out << static_cast<char>(ch);
        }
    }
    out << '"';
    return out.str();
}

std::string join(const std::vector<std::string>& values, const std::string& sep) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << sep;
        out << values[i];
    }
    return out.str();
}

std::vector<std::string> split(const std::string& line, char delim) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto pos = line.find(delim, start);
        out.push_back(line.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}

PublicEvent parse_event(const std::vector<std::string>& f, std::size_t line_number) {
    if (f.size() != 12)
        throw std::runtime_error("event line " + std::to_string(line_number) +
                                 ": expected 12 columns, got " + std::to_string(f.size()));
    PublicEvent event;
    event.event_id = std::stoull(f[0]);
    event.session_id = std::stoull(f[1]);
    event.event_type = f[2];
    event.actor = std::stoi(f[3]);
    if (f[4] != "-" && !f[4].empty()) event.public_tiles = split(f[4], ';');
    event.public_action = f[5] == "-" ? "" : f[5];
    event.speaker = std::stoi(f[6]);
    event.persona_id = f[7];
    event.emotion = f[8];
    event.fact_key = f[9];
    event.timestamp_ms = std::stoll(f[10]);
    event.note = f[11];
    return event;
}

std::vector<PublicEvent> load_events(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open events: " + path);
    std::vector<PublicEvent> events;
    std::string line;
    std::size_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        events.push_back(parse_event(split(line, '\t'), number));
    }
    if (events.empty()) throw std::runtime_error("no events in " + path);
    return events;
}

std::string public_facts(const PublicEvent& event) {
    std::ostringstream out;
    out << event.event_type;
    if (!event.public_tiles.empty()) out << " tiles=" << join(event.public_tiles, ",");
    if (!event.public_action.empty()) out << " action=" << event.public_action;
    out << " fact=" << event.fact_key;
    return out.str();
}

std::string build_prompt(const PublicEvent& event, const std::vector<const BankEntry*>& candidates) {
    std::ostringstream out;
    out << "你是单机麻将的台词候选选择器。只能输出一个数字：1、2 或 3。\n"
        << "公开事件：" << public_facts(event) << "。\n"
        << "说话角色：" << event.persona_id << "；情绪：" << event.emotion << "。\n"
        << "候选台词：\n";
    const auto count = std::min<std::size_t>(3, candidates.size());
    for (std::size_t i = 0; i < count; ++i)
        out << (i + 1) << ". " << candidates[i]->text << "\n";
    out << "只回复编号，不要解释。";
    return out.str();
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    if (values.size() == 1) return values[0];
    const double rank = (p / 100.0) * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(rank));
    const auto hi = static_cast<std::size_t>(std::ceil(rank));
    if (lo == hi) return values[lo];
    return values[lo] + (values[hi] - values[lo]) * (rank - static_cast<double>(lo));
}

double peak_rss_mb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
#endif
    return 0.0;
}

double process_cpu_ms() {
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        auto to_ms = [](const FILETIME& t) {
            return (static_cast<double>(t.dwHighDateTime) * 4294967296.0 +
                    static_cast<double>(t.dwLowDateTime)) / 10000.0;
        };
        return to_ms(kernel) + to_ms(user);
    }
#endif
    return 0.0;
}

struct Options {
    std::string bank{"benchmarks/p2_bank_manifest.tsv"};
    std::string events{"benchmarks/p2_events.tsv"};
    std::string audio_dir{"benchmarks/results/p2/audio/bank_audio"};
    std::string strategy{"fixed"};
    std::string out{"benchmarks/results/p2/runs"};
    std::string run_id{"p2"};
    std::string coverage;
    std::string mnn_config;
    int max_tokens{8};
    bool play{false};
    bool force{false};
    std::size_t limit{0};
};

Options parse(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--bank") opt.bank = need();
        else if (arg == "--events") opt.events = need();
        else if (arg == "--audio-dir") opt.audio_dir = need();
        else if (arg == "--strategy") opt.strategy = need();
        else if (arg == "--out") opt.out = need();
        else if (arg == "--run-id") opt.run_id = need();
        else if (arg == "--coverage") opt.coverage = need();
        else if (arg == "--mnn-config") opt.mnn_config = need();
        else if (arg == "--max-tokens") opt.max_tokens = std::stoi(need());
        else if (arg == "--limit") opt.limit = static_cast<std::size_t>(std::stoul(need()));
        else if (arg == "--play") opt.play = true;
        else if (arg == "--force") opt.force = true;
        else throw std::runtime_error("unknown argument: " + arg);
    }
    if (opt.strategy != "fixed" && opt.strategy != "weighted" && opt.strategy != "llm")
        throw std::runtime_error("strategy must be fixed, weighted or llm");
    return opt;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options opt = parse(argc, argv);
        std::string error;
        auto bank = Bank::load(opt.bank, error);
        if (!bank) throw std::runtime_error(error);
        const auto events = load_events(opt.events);

        std::unique_ptr<aimj::IAudioPlayer> player;
        if (opt.play) player = aimj::make_audio_player();
        std::unique_ptr<aimj::ILlmBackend> llm;
        if (opt.strategy == "llm") {
#ifdef AIMJ_WITH_MNN
            if (opt.mnn_config.empty())
                throw std::runtime_error("--strategy llm requires --mnn-config");
            llm = aimj::make_mnn_backend(opt.mnn_config, opt.max_tokens);
#else
            throw std::runtime_error("this build has no MNN backend; rebuild with AIMJ_WITH_MNN=ON");
#endif
        }

        const fs::path out = fs::path(opt.out) / opt.run_id;
        fs::create_directories(out);
        const fs::path results = out / "results.jsonl";
        if (fs::exists(results) && !opt.force)
            throw std::runtime_error("results.jsonl exists (use --force): " + results.string());
        std::ofstream log(results, std::ios::binary | std::ios::trunc);
        if (!log) throw std::runtime_error("cannot open " + results.string());
        log << std::fixed << std::setprecision(3);

        Director director(*bank);
        std::map<std::string, std::string> hash_cache;
        std::map<std::string, int> line_counts;
        std::string last_line;
        int consecutive = 0;
        int max_consecutive = 0;
        std::size_t delivered = 0, no_candidate = 0, rejected = 0, played = 0;
        std::size_t player_unavailable = 0, llm_calls = 0, llm_valid = 0;
        std::size_t factual_violations = 0, sid_mismatches = 0, fallback_used = 0;
        std::vector<double> pcm_times, submit_times, callback_times, llm_times;

        const std::size_t total = opt.limit ? std::min(opt.limit, events.size()) : events.size();
        for (std::size_t index = 0; index < total; ++index) {
            const PublicEvent& event = events[index];
            const auto start = Clock::now();
            double last_llm_ms = 0.0;
            Selection selection;
            if (opt.strategy == "llm" && llm && !bank->candidates(event).empty()) {
                ++llm_calls;
                const auto llm_start = Clock::now();
                selection = director.select(event, opt.strategy,
                    [&](const PublicEvent& e, const std::vector<const BankEntry*>& candidates) {
                        return llm->generate(build_prompt(e, candidates), e.speaker);
                    });
                last_llm_ms = ms_since(llm_start);
                llm_times.push_back(last_llm_ms);
                selection.llm_ms = last_llm_ms;
                if (selection.llm_valid) ++llm_valid;
            } else {
                selection = director.select(event, opt.strategy);
            }

            std::string validation = "not_applicable";
            std::string audio_status = "skipped";
            bool has_pcm = false;
            bool fallback = false;
            std::string audio_error;
            double event_to_pcm_ms = 0.0;
            aimj::PlaybackTiming playback;
            if (!selection.entry) {
                ++no_candidate;
                ++fallback_used;
                fallback = true;
            } else {
                const auto& entry = *selection.entry;
                const auto check = aimj::voice::validate(event, entry);
                if (!check.ok) {
                    validation = "rejected";
                    ++rejected;
                    ++factual_violations;
                    ++fallback_used;
                    fallback = true;
                    audio_status = "rejected_before_tts";
                    std::vector<std::string> errors = check.errors;
                    audio_error = join(errors, "; ");
                } else if (aimj::voice::sid_map(event.speaker) != std::to_string(entry.tts_sid)) {
                    validation = "sid_mismatch";
                    ++sid_mismatches;
                    ++rejected;
                    ++fallback_used;
                    fallback = true;
                    audio_status = "rejected_before_tts";
                    audio_error = "sid does not match speaker";
                } else {
                    validation = "ok";
                    const fs::path audio_path = fs::path(opt.audio_dir) / entry.audio_file;
                    try {
                        auto cached = hash_cache.find(entry.line_id);
                        if (cached == hash_cache.end()) {
                            const auto digest = aimj_probe::Sha256::file_hex(audio_path.string());
                            if (digest.empty()) throw std::runtime_error("cannot read audio hash");
                            cached = hash_cache.emplace(entry.line_id, digest).first;
                        }
                        if (!entry.audio_sha256.empty() && cached->second != entry.audio_sha256)
                            throw std::runtime_error("audio hash mismatch");
                        const auto wav = aimj::read_pcm16_wav(audio_path.string());
                        if (wav.audio.sample_rate != 8000)
                            throw std::runtime_error("unexpected sample rate");
                        event_to_pcm_ms = ms_since(start);
                        pcm_times.push_back(event_to_pcm_ms);
                        has_pcm = true;
                        ++delivered;
                        audio_status = "ready";
                        if (player) {
                            playback = player->play(wav.audio, audio_path.string());
                            if (playback.submitted) {
                                ++played;
                                submit_times.push_back(playback.pcm_to_submit_ms);
                                callback_times.push_back(playback.submit_to_callback_ms);
                                audio_status = playback.status;
                            } else {
                                ++player_unavailable;
                                audio_status = "player_unavailable";
                                audio_error = playback.error;
                            }
                        }
                    } catch (const std::exception& exc) {
                        validation = "audio_rejected";
                        ++rejected;
                        ++fallback_used;
                        fallback = true;
                        audio_status = "rejected_before_play";
                        audio_error = exc.what();
                    }
                }
            }

            const std::string line_id = selection.entry ? selection.entry->line_id : "";
            if (!line_id.empty()) {
                ++line_counts[line_id];
                if (line_id == last_line) ++consecutive;
                else { consecutive = 1; last_line = line_id; }
                max_consecutive = std::max(max_consecutive, consecutive);
            } else {
                last_line.clear();
                consecutive = 0;
            }

            log << "{\"type\":\"event\",\"schema\":1"
                << ",\"run_id\":" << json(opt.run_id)
                << ",\"strategy\":" << json(opt.strategy)
                << ",\"event_id\":" << event.event_id
                << ",\"session_id\":" << event.session_id
                << ",\"event_type\":" << json(event.event_type)
                << ",\"speaker\":" << event.speaker
                << ",\"persona_id\":" << json(event.persona_id)
                << ",\"emotion\":" << json(event.emotion)
                << ",\"public_facts\":" << json(public_facts(event))
                << ",\"fact_key\":" << json(event.fact_key)
                << ",\"selected_line_id\":" << json(line_id)
                << ",\"selection_method\":" << json(selection.method)
                << ",\"candidate_count\":" << selection.candidate_count
                << ",\"llm_valid\":" << (selection.llm_valid ? "true" : "false")
                << ",\"llm_raw\":" << json(selection.llm_raw)
                << ",\"llm_ms\":" << selection.llm_ms
                << ",\"approved_text\":" << json(selection.entry ? selection.entry->text : "")
                << ",\"tts_sid\":" << (selection.entry ? selection.entry->tts_sid : 0)
                << ",\"cache_hit\":" << (selection.entry ? "true" : "false")
                << ",\"fallback_used\":" << (fallback ? "true" : "false")
                << ",\"validation_result\":" << json(validation)
                << ",\"validation_errors\":" << json(audio_error)
                << ",\"audio_status\":" << json(audio_status)
                << ",\"event_to_pcm_ms\":" << (has_pcm ? std::to_string(event_to_pcm_ms) : std::string("null"))
                << ",\"pcm_to_player_submit_ms\":" << playback.pcm_to_submit_ms
                << ",\"player_callback_ms\":" << playback.submit_to_callback_ms
                << ",\"audio_duration_ms\":" << playback.audio_duration_ms
                << ",\"first_audio_output_ms\":null"
                << ",\"first_audible_ms\":null"
                << ",\"first_audible_reason\":" << json("waveOut submit/callback is not an audible-onset measurement")
                << ",\"timestamp_ms\":" << event.timestamp_ms
                << "}\n";
            log.flush();
            std::cout << '[' << (index + 1) << '/' << total << "] " << event.event_type
                      << " s" << event.speaker << " " << selection.method << " "
                      << (selection.entry ? selection.entry->line_id : "(silence)")
                      << " " << audio_status << '\n';
        }

        std::size_t covered = 0;
        std::size_t fixtures = 0;
        std::vector<std::uint64_t> missing;
        if (!opt.coverage.empty()) {
            const auto bench = load_events(opt.coverage);
            fixtures = bench.size();
            for (const auto& event : bench) {
                if (!bank->candidates(event).empty()) ++covered;
                else missing.push_back(event.event_id);
            }
        }

        log << "{\"type\":\"summary\",\"schema\":1"
            << ",\"run_id\":" << json(opt.run_id)
            << ",\"strategy\":" << json(opt.strategy)
            << ",\"events\":" << total
            << ",\"delivered\":" << delivered
            << ",\"no_candidate\":" << no_candidate
            << ",\"validation_rejected\":" << rejected
            << ",\"factual_violations\":" << factual_violations
            << ",\"sid_mismatches\":" << sid_mismatches
            << ",\"fallback_used\":" << fallback_used
            << ",\"unique_lines\":" << line_counts.size()
            << ",\"max_consecutive_same\":" << max_consecutive
            << ",\"played\":" << played
            << ",\"player_unavailable\":" << player_unavailable
            << ",\"event_to_pcm_p50_ms\":" << percentile(pcm_times, 50)
            << ",\"event_to_pcm_p95_ms\":" << percentile(pcm_times, 95)
            << ",\"pcm_to_submit_p50_ms\":" << percentile(submit_times, 50)
            << ",\"pcm_to_submit_p95_ms\":" << percentile(submit_times, 95)
            << ",\"callback_p50_ms\":" << percentile(callback_times, 50)
            << ",\"callback_p95_ms\":" << percentile(callback_times, 95)
            << ",\"llm_calls\":" << llm_calls
            << ",\"llm_valid\":" << llm_valid
            << ",\"llm_invalid\":" << (llm_calls - llm_valid)
            << ",\"llm_p50_ms\":" << percentile(llm_times, 50)
            << ",\"llm_p95_ms\":" << percentile(llm_times, 95)
            << ",\"peak_rss_mb\":" << peak_rss_mb()
            << ",\"process_cpu_ms\":" << process_cpu_ms()
            << ",\"bench_fixtures\":" << fixtures
            << ",\"bench_covered\":" << covered
            << ",\"bench_missing\":[";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i) log << ',';
            log << missing[i];
        }
        log << "],\"first_audible_ms\":null"
            << ",\"first_audible_reason\":" << json("no loopback capture and no human ear confirmation")
            << "}\n";
        log.flush();
        std::cout << "Results: " << results.string() << "; delivered=" << delivered
                  << "; no_candidate=" << no_candidate << "; rejected=" << rejected
                  << "; max_consecutive=" << max_consecutive << '\n';
        return rejected == 0 ? 0 : 2;
    } catch (const std::exception& exc) {
        std::cerr << "ERROR: " << exc.what() << '\n';
        return 1;
    }
}
