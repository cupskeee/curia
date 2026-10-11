// SPDX-License-Identifier: MIT
#include "providers/provider_util.h"

namespace curia::providers {

namespace {

bool isContinuation(unsigned char c) {
    return c >= 0x80 && c <= 0xBF;
}

// The length of the valid UTF-8 sequence that starts at text[at] (2 to 4), or 0 if there is none
// (a stray continuation byte, an overlong form, a surrogate, a value above U+10FFFF, or a
// sequence cut off by the end of the text). `at` must be a non-ASCII byte.
std::size_t utf8SequenceLength(std::string_view text, std::size_t at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = 0;
    unsigned char lowest = 0x80;
    unsigned char highest = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        lowest = lead == 0xE0 ? 0xA0 : 0x80;
        highest = lead == 0xED ? 0x9F : 0xBF;  // no surrogates
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        lowest = lead == 0xF0 ? 0x90 : 0x80;
        highest = lead == 0xF4 ? 0x8F : 0xBF;  // nothing above U+10FFFF
    } else {
        return 0;
    }
    if (at + length > text.size()) {
        return 0;
    }
    const auto second = static_cast<unsigned char>(text[at + 1]);
    if (second < lowest || second > highest) {
        return 0;
    }
    for (std::size_t i = 2; i < length; ++i) {
        if (!isContinuation(static_cast<unsigned char>(text[at + i]))) {
            return 0;
        }
    }
    return length;
}

// True if `text` nests arrays and objects no deeper than `maxDepth`. Brackets inside strings do
// not count. Unbalanced closers are tolerated here (the parser rejects them later).
bool depthWithin(std::string_view text, int maxDepth) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > maxDepth) {
                return false;
            }
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return true;
}

}  // namespace

std::optional<nlohmann::json> parseBoundedJson(std::string_view text, std::size_t maxBytes,
                                               int maxDepth) {
    if (text.size() > maxBytes || !depthWithin(text, maxDepth)) {
        return std::nullopt;
    }
    // allow_exceptions=false: a failure yields a discarded value instead of a throw.
    nlohmann::json value = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (value.is_discarded()) {
        return std::nullopt;
    }
    return value;
}

std::string printableExcerpt(std::string_view text, std::size_t maxBytes) {
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto c = static_cast<unsigned char>(text[at]);
        std::size_t take = 1;
        char single = '?';
        std::string_view whole;
        if (c < 0x80) {
            single = (c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
        } else if (const std::size_t length = utf8SequenceLength(text, at); length != 0) {
            take = length;
            whole = text.substr(at, length);
            // C1 controls (U+0080 to U+009F) are two bytes starting with 0xC2.
            if (c == 0xC2 && static_cast<unsigned char>(text[at + 1]) < 0xA0) {
                single = ' ';
                whole = {};
            }
        }
        const std::size_t added = whole.empty() ? 1 : whole.size();
        if (out.size() + added > maxBytes) {
            break;
        }
        if (whole.empty()) {
            out.push_back(single);
        } else {
            out.append(whole);
        }
        at += take;
    }
    const std::size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    out.erase(0, first);
    out.erase(out.find_last_not_of(' ') + 1);
    return out;
}

}  // namespace curia::providers
