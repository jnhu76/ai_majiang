#pragma once
#include <cstddef>
#include <string>
#include <vector>
namespace aimj {
// Per-keyword audit of generated dialogue against its review hints. The scorer
// is negation aware: a correct refusal such as "不能胡" or "还没结算" contains a
// forbidden word but must not be counted as a violation. It never decides a
// GO/NO-GO by itself; it records raw/negated/violating hits, whether the
// keyword also appears in the prompt (a model repeating prompt material is a
// prompt echo, not new speech) and the surrounding text for human review.
struct KeywordAudit {
    std::string keyword;
    int raw_hits{};
    int negated_hits{};   // hit preceded within 4 code points by 不/没/无/别/非/未
    int violating_hits{}; // raw - negated
    bool in_prompt{};
    bool echo_suspect{};  // hit sits inside a >=6 code point verbatim prompt echo
    std::vector<std::string> contexts; // +/-12 code points around each raw hit
};
struct TextAudit {
    std::size_t codepoints{};
    std::size_t prompt_overlap_codepoints{}; // longest common substring with prompt
    std::vector<KeywordAudit> required;
    std::vector<KeywordAudit> forbidden;
    int required_missing{};        // required keywords with zero hits
    int required_echo_suspect{};   // required hits that are prompt echoes
    // Gate count for forbidden hits: non-negated, not an echo and the keyword
    // does not appear in the prompt. Excluded hits stay visible in `forbidden`
    // so a human can review them instead of them passing silently.
    int violating_gate_hits{};
    int violating_prompt_echo_hits{};
};
// ';' separated keyword lists, as stored in the dialogue fixture TSV.
TextAudit score_text(const std::string& text, const std::string& prompt,
                     const std::string& required_kw, const std::string& forbidden_kw);
std::u32string utf8_to_codepoints(const std::string& text);
std::string codepoints_to_utf8(const std::u32string& text);
std::size_t longest_common_substring(const std::u32string& a, const std::u32string& b);
} // namespace aimj
