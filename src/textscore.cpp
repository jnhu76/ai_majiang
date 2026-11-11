#include "aimj/textscore.hpp"
#include <algorithm>
namespace aimj {
namespace {
std::u32string decode_utf8(const std::string& s) {
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
std::string encode_utf8(const std::u32string& s) {
    std::string out;
    for (char32_t cp : s) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}
bool is_negator(char32_t cp) {
    switch (cp) {
    case U'不': case U'没': case U'无': case U'别': case U'非': case U'未':
        return true;
    default:
        return false;
    }
}
std::vector<std::string> split_keywords(const std::string& list) {
    std::vector<std::string> out;
    std::size_t begin = 0;
    for (std::size_t p = 0; p <= list.size(); ++p) {
        if (p == list.size() || list[p] == ';') {
            if (p > begin) out.push_back(list.substr(begin, p - begin));
            begin = p + 1;
        }
    }
    return out;
}
std::vector<std::size_t> find_all(const std::u32string& haystack, const std::u32string& needle) {
    std::vector<std::size_t> hits;
    if (needle.empty() || needle.size() > haystack.size()) return hits;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (std::equal(needle.begin(), needle.end(), haystack.begin() + static_cast<std::ptrdiff_t>(i)))
            hits.push_back(i);
    }
    return hits;
}
KeywordAudit audit_keyword(const std::u32string& text_cp,
                           const std::string& prompt, const std::string& keyword) {
    KeywordAudit a;
    a.keyword = keyword;
    a.in_prompt = !keyword.empty() && prompt.find(keyword) != std::string::npos;
    const std::u32string kw = decode_utf8(keyword);
    const auto hits = find_all(text_cp, kw);
    a.raw_hits = static_cast<int>(hits.size());
    for (std::size_t pos : hits) {
        bool negated = false;
        for (std::size_t back = 1; back <= 4 && back <= pos; ++back)
            if (is_negator(text_cp[pos - back])) { negated = true; break; }
        if (negated) ++a.negated_hits;
        // +/-12 code point excerpt around the hit for manual review.
        const std::size_t lo = pos > 12 ? pos - 12 : 0;
        const std::size_t hi = std::min(text_cp.size(), pos + kw.size() + 12);
        a.contexts.push_back(encode_utf8(text_cp.substr(lo, hi - lo)));
        // A short window around the hit that also occurs verbatim in the prompt
        // means the model is repeating prompt material, not describing the state.
        const std::size_t wlo = pos > 2 ? pos - 2 : 0;
        const std::size_t whi = std::min(text_cp.size(), pos + kw.size() + 2);
        if (whi - wlo >= 6) {
            const std::string window = encode_utf8(text_cp.substr(wlo, whi - wlo));
            if (prompt.find(window) != std::string::npos) a.echo_suspect = true;
        }
    }
    a.violating_hits = a.raw_hits - a.negated_hits;
    return a;
}
} // namespace
std::u32string utf8_to_codepoints(const std::string& text) { return decode_utf8(text); }
std::string codepoints_to_utf8(const std::u32string& text) { return encode_utf8(text); }
std::size_t longest_common_substring(const std::u32string& a, const std::u32string& b) {
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
TextAudit score_text(const std::string& text, const std::string& prompt,
                     const std::string& required_kw, const std::string& forbidden_kw) {
    TextAudit out;
    const auto text_cp = decode_utf8(text);
    out.codepoints = text_cp.size();
    out.prompt_overlap_codepoints = longest_common_substring(text_cp, decode_utf8(prompt));
    for (const auto& kw : split_keywords(required_kw)) {
        out.required.push_back(audit_keyword(text_cp, prompt, kw));
        const auto& a = out.required.back();
        if (a.raw_hits == 0) ++out.required_missing;
        if (a.raw_hits > 0 && a.echo_suspect) ++out.required_echo_suspect;
    }
    for (const auto& kw : split_keywords(forbidden_kw)) {
        out.forbidden.push_back(audit_keyword(text_cp, prompt, kw));
        const auto& a = out.forbidden.back();
        if (a.violating_hits <= 0) continue;
        if (!a.in_prompt && !a.echo_suspect) out.violating_gate_hits += a.violating_hits;
        else out.violating_prompt_echo_hits += a.violating_hits;
    }
    return out;
}
} // namespace aimj
