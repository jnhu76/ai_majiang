#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aimj::voice {
// Public event contract: every field is supplied by the event producer and is
// already public to the table. No hidden hand, wall order, score or legal-action
// set is carried here; the rules layer remains the sole authority.
struct PublicEvent {
    std::uint64_t event_id{};
    std::uint64_t session_id{};
    std::string event_type;
    int actor{};
    std::vector<std::string> public_tiles;
    std::string public_action;
    int speaker{};
    std::string persona_id;
    std::string emotion;
    std::string fact_key;
    std::int64_t timestamp_ms{};
    std::string note;
};

struct BankEntry {
    std::string line_id;
    std::string event_type;
    std::string fact_key;
    int speaker{};
    std::string persona_id;
    std::string emotion;
    std::string text;
    int tts_sid{};
    std::string audio_file;
    std::string audio_sha256;
    int weight{1};
};

struct Selection {
    const BankEntry* entry{};
    std::string method;  // fixed | weighted | llm | llm_fallback | no_candidate
    std::string reason;
    int candidate_count{};
    bool llm_valid{};
    std::string llm_raw;
    double llm_ms{};
};

struct Validation {
    bool ok{};
    std::vector<std::string> errors;
};

class Bank {
public:
    static std::optional<Bank> load(const std::string& manifest_path, std::string& error);
    // Exact (event_type, fact_key, speaker). There is deliberately no
    // event-type-only fallback: a mismatched public fact must not hit.
    std::vector<const BankEntry*> candidates(const PublicEvent& event) const;
    const BankEntry* find(const std::string& line_id) const;
    const std::vector<BankEntry>& entries() const { return entries_; }
private:
    std::vector<BankEntry> entries_;
};

Validation validate(const PublicEvent& event, const BankEntry& entry);

class Director {
public:
    using LlmPicker = std::function<std::string(
        const PublicEvent&, const std::vector<const BankEntry*>&)>;
    explicit Director(const Bank& bank) : bank_(bank) {}
    Selection select(const PublicEvent& event, const std::string& strategy,
                     const LlmPicker& llm_pick = {});
    // A no-candidate event is silence, not free generation: this is the only
    // safe fallback until a curated line exists.
    static std::string fallback_line(const PublicEvent&) { return {}; }
private:
    std::string key_for(const PublicEvent& event) const;
    bool recently_used(const std::string& line_id) const;
    const BankEntry* deterministic(const PublicEvent& event,
                                   const std::vector<const BankEntry*>& candidates,
                                   const std::string& method, std::string& reason);
    const Bank& bank_;
    std::map<std::string, std::size_t> rotation_;
    std::deque<std::string> recent_;
};

// UTF-8 helpers used by validation and the report.
std::size_t codepoints(const std::string& text);
bool contains_tile_token(const std::string& text, const std::string& tile);
std::string sid_map(int speaker);
} // namespace aimj::voice
