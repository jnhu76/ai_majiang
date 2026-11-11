#include "aimj/voice_loop.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace aimj::voice {
namespace {
const char* const kTiles[] = {
    "一万", "二万", "三万", "四万", "五万", "六万", "七万", "八万", "九万",
    "一条", "二条", "三条", "四条", "五条", "六条", "七条", "八条", "九条",
    "一筒", "二筒", "三筒", "四筒", "五筒", "六筒", "七筒", "八筒", "九筒",
    "东", "南", "西", "北", "红中", "发财", "白板"};

std::vector<std::string> split_tab(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return out;
}

std::string trim_cr(std::string value) {
    if (!value.empty() && value.back() == '\r') value.pop_back();
    return value;
}

std::string trim_space(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

bool hidden_or_untrusted(const std::string& fact_key) {
    return fact_key.rfind("hidden:", 0) == 0 || fact_key.rfind("chat:", 0) == 0;
}

bool fixed_line(const std::string& fact_key) {
    return fact_key.rfind("fixed:", 0) == 0;
}

} // namespace

std::size_t codepoints(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char ch : text)
        if ((ch & 0xC0u) != 0x80u) ++count;
    return count;
}

bool contains_tile_token(const std::string& text, const std::string& tile) {
    return !tile.empty() && text.find(tile) != std::string::npos;
}

std::string sid_map(int speaker) {
    switch (speaker) {
        case 1: return "8";
        case 2: return "9";
        case 3: return "10";
        default: return "?";
    }
}

std::optional<Bank> Bank::load(const std::string& manifest_path, std::string& error) {
    std::ifstream in(manifest_path, std::ios::binary);
    if (!in) {
        error = "cannot open bank manifest: " + manifest_path;
        return std::nullopt;
    }
    Bank bank;
    std::string line;
    std::size_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        line = trim_cr(line);
        if (line.empty() || line[0] == '#') continue;
        const auto f = split_tab(line);
        if (f.size() != 11) {
            error = "bank line " + std::to_string(number) + ": expected 11 columns, got " +
                    std::to_string(f.size());
            return std::nullopt;
        }
        try {
            BankEntry entry;
            entry.line_id = f[0];
            entry.event_type = f[1];
            entry.fact_key = f[2];
            entry.speaker = std::stoi(f[3]);
            entry.persona_id = f[4];
            entry.emotion = f[5];
            entry.text = f[6];
            entry.tts_sid = std::stoi(f[7]);
            entry.audio_file = f[8];
            entry.audio_sha256 = f[9];
            entry.weight = std::stoi(f[10]);
            if (entry.line_id.empty() || entry.event_type.empty() || entry.fact_key.empty() ||
                entry.text.empty() || entry.audio_file.empty()) {
                error = "bank line " + std::to_string(number) + ": empty required field";
                return std::nullopt;
            }
            const auto expected_sid = sid_map(entry.speaker);
            if (expected_sid == "?" || std::to_string(entry.tts_sid) != expected_sid) {
                error = "bank line " + std::to_string(number) + ": sid " +
                        std::to_string(entry.tts_sid) + " does not match speaker " +
                        std::to_string(entry.speaker);
                return std::nullopt;
            }
            bank.entries_.push_back(std::move(entry));
        } catch (const std::exception&) {
            error = "bank line " + std::to_string(number) + ": malformed number";
            return std::nullopt;
        }
    }
    if (bank.entries_.empty()) {
        error = "bank manifest is empty: " + manifest_path;
        return std::nullopt;
    }
    return bank;
}

std::vector<const BankEntry*> Bank::candidates(const PublicEvent& event) const {
    std::vector<const BankEntry*> result;
    for (const auto& entry : entries_) {
        if (entry.event_type == event.event_type && entry.fact_key == event.fact_key &&
            entry.speaker == event.speaker) {
            result.push_back(&entry);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto* a, const auto* b) {
        return a->line_id < b->line_id;
    });
    return result;
}

const BankEntry* Bank::find(const std::string& line_id) const {
    for (const auto& entry : entries_)
        if (entry.line_id == line_id) return &entry;
    return nullptr;
}

Validation validate(const PublicEvent& event, const BankEntry& entry) {
    Validation out;
    auto fail = [&](std::string message) { out.errors.push_back(std::move(message)); };
    if (entry.event_type != event.event_type) fail("event_type mismatch");
    if (entry.fact_key != event.fact_key) fail("fact_key mismatch");
    if (entry.speaker != event.speaker) fail("speaker mismatch");
    if (entry.persona_id != event.persona_id) fail("persona mismatch");
    if (std::to_string(entry.tts_sid) != sid_map(event.speaker)) fail("tts_sid/speaker mismatch");
    const auto length = codepoints(entry.text);
    if (length < 2 || length > 40) fail("line length outside 2..40 code points");
    for (const char* tile : kTiles) {
        if (!contains_tile_token(entry.text, tile)) continue;
        const bool public_tile = std::find(event.public_tiles.begin(), event.public_tiles.end(),
                                           tile) != event.public_tiles.end();
        if (!public_tile) fail(std::string("non-public tile mentioned: ") + tile);
    }
    if (!hidden_or_untrusted(event.fact_key) && !fixed_line(event.fact_key)) {
        if (entry.text.find("暗牌") != std::string::npos ||
            entry.text.find("你手里") != std::string::npos ||
            entry.text.find("我看见") != std::string::npos) {
            fail("hidden-state wording outside a hidden/chat event");
        }
    }
    if (!hidden_or_untrusted(event.fact_key) && !fixed_line(event.fact_key)) {
        if (entry.text.find("胡") != std::string::npos ||
            entry.text.find("自摸") != std::string::npos ||
            entry.text.find("赢了") != std::string::npos ||
            entry.text.find("分数") != std::string::npos) {
            fail("win/score claim outside a hidden/chat/fixed event");
        }
    }
    out.ok = out.errors.empty();
    return out;
}

std::string Director::key_for(const PublicEvent& event) const {
    return event.event_type + "|" + event.fact_key + "|" + std::to_string(event.speaker) +
           "|" + event.persona_id;
}

bool Director::recently_used(const std::string& line_id) const {
    return std::find(recent_.begin(), recent_.end(), line_id) != recent_.end();
}

const BankEntry* Director::deterministic(const PublicEvent& event,
                                         const std::vector<const BankEntry*>& candidates,
                                         const std::string& method, std::string& reason) {
    if (candidates.empty()) return nullptr;
    const auto key = key_for(event);
    auto& rotation = rotation_[key];
    const auto start = rotation % candidates.size();
    ++rotation;
    const BankEntry* chosen = candidates[start];
    // Prefer a line that was not just used; the four-line recent window is
    // enough to stop an audible short loop without starving small banks.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto* candidate = candidates[(start + i) % candidates.size()];
        if (!recently_used(candidate->line_id)) { chosen = candidate; break; }
    }
    if (!reason.empty()) reason += "; ";
    reason += method + " rotation";
    return chosen;
}

Selection Director::select(const PublicEvent& event, const std::string& strategy,
                           const LlmPicker& llm_pick) {
    Selection selection;
    const auto candidates = bank_.candidates(event);
    selection.candidate_count = static_cast<int>(candidates.size());
    if (candidates.empty()) {
        selection.method = "no_candidate";
        selection.reason = "no entry for exact (event_type, fact_key, speaker)";
        return selection;
    }
    if (strategy == "fixed" || strategy == "weighted") {
        if (strategy == "fixed") {
            selection.entry = deterministic(event, candidates, "fixed", selection.reason);
            selection.method = "fixed";
        } else {
            // Weight + emotion/persona match, with a hard penalty for a line
            // that is still in the recent window.
            const BankEntry* best = nullptr;
            int best_score = -1000000;
            for (const auto* candidate : candidates) {
                int score = candidate->weight;
                if (candidate->emotion == event.emotion) score += 3;
                if (candidate->persona_id == event.persona_id) score += 1;
                if (recently_used(candidate->line_id)) score -= 1000;
                if (!best || score > best_score ||
                    (score == best_score && candidate->line_id < best->line_id)) {
                    best = candidate;
                    best_score = score;
                }
            }
            selection.entry = best;
            selection.method = "weighted";
            selection.reason = "weighted score, recent lines penalized";
        }
    } else if (strategy == "llm") {
        if (!llm_pick) {
            selection.method = "llm_fallback";
            selection.reason = "llm strategy requested without a picker";
            selection.entry = deterministic(event, candidates, "fixed", selection.reason);
        } else {
            const std::string raw = trim_space(llm_pick(event, candidates));
            selection.llm_raw = raw;
            int pick = 0;
            if (!raw.empty() && std::isdigit(static_cast<unsigned char>(raw.front())))
                pick = raw.front() - '0';
            if (pick >= 1 && pick <= static_cast<int>(candidates.size())) {
                const auto* picked = candidates[static_cast<std::size_t>(pick - 1)];
                if (candidates.size() > 1 && recently_used(picked->line_id)) {
                    // The model's id is legal but would repeat a line that is
                    // still audible in the recent window; deterministic
                    // alternation keeps the anti-loop contract.
                    selection.method = "llm_recent_override";
                    selection.reason = "model picked a recently used line; deterministic alternate";
                    selection.llm_valid = true;
                    selection.entry = deterministic(event, candidates, "fixed", selection.reason);
                } else {
                    selection.entry = picked;
                    selection.method = "llm";
                    selection.reason = "model selected a valid candidate id";
                    selection.llm_valid = true;
                }
            } else {
                selection.method = "llm_fallback";
                selection.reason = "model output outside 1.." + std::to_string(candidates.size());
                selection.entry = deterministic(event, candidates, "fixed", selection.reason);
            }
        }
    } else {
        selection.method = "fixed";
        selection.reason = "unknown strategy, fixed rotation";
        selection.entry = deterministic(event, candidates, "fixed", selection.reason);
    }
    if (selection.entry) {
        recent_.push_back(selection.entry->line_id);
        while (recent_.size() > 4) recent_.pop_front();
    }
    return selection;
}
} // namespace aimj::voice
