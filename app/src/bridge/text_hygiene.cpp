// SPDX-License-Identifier: MIT
#include "bridge/text_hygiene.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace curia::bridge {

namespace {

using CodePoint = std::uint32_t;

constexpr CodePoint kReplacement = 0xFFFD;
constexpr CodePoint kLinkMarker = 0x15;
constexpr CodePoint kLinkEnd = 0x21;
constexpr CodePoint kSpace = 0x20;

// Decodes the sequence at `i` and advances `i` past it (or past its maximal valid prefix).
CodePoint decodeOne(std::string_view s, std::size_t& i) {
    const unsigned b0 = static_cast<unsigned char>(s[i]);
    ++i;
    if (b0 < 0x80U) {
        return b0;
    }
    unsigned needed = 0;
    unsigned lowest = 0x80U;
    unsigned highest = 0xBFU;
    CodePoint cp = 0;
    if (b0 >= 0xC2U && b0 <= 0xDFU) {
        needed = 1;
        cp = b0 & 0x1FU;
    } else if (b0 >= 0xE0U && b0 <= 0xEFU) {
        needed = 2;
        cp = b0 & 0x0FU;
        if (b0 == 0xE0U) {
            lowest = 0xA0U;  // no overlong forms
        }
        if (b0 == 0xEDU) {
            highest = 0x9FU;  // no surrogates
        }
    } else if (b0 >= 0xF0U && b0 <= 0xF4U) {
        needed = 3;
        cp = b0 & 0x07U;
        if (b0 == 0xF0U) {
            lowest = 0x90U;
        }
        if (b0 == 0xF4U) {
            highest = 0x8FU;  // nothing above U+10FFFF
        }
    } else {
        return kReplacement;
    }
    for (unsigned k = 0; k < needed; ++k) {
        if (i >= s.size()) {
            return kReplacement;
        }
        const unsigned b = static_cast<unsigned char>(s[i]);
        if (b < lowest || b > highest) {
            return kReplacement;  // the offending byte starts the next sequence
        }
        lowest = 0x80U;
        highest = 0xBFU;
        cp = (cp << 6) | (b & 0x3FU);
        ++i;
    }
    return cp;
}

std::vector<CodePoint> decode(std::string_view s) {
    std::vector<CodePoint> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        out.push_back(decodeOne(s, i));
    }
    return out;
}

void appendUtf8(std::string& out, CodePoint cp) {
    if (cp < 0x80U) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

bool startsWith(const std::vector<CodePoint>& v, std::size_t from, std::size_t to,
                std::string_view prefix) {
    if (to - from < prefix.size()) {
        return false;
    }
    for (std::size_t k = 0; k < prefix.size(); ++k) {
        if (v[from + k] != static_cast<CodePoint>(static_cast<unsigned char>(prefix[k]))) {
            return false;
        }
    }
    return true;
}

// The tag word is v[from, to): exactly "L" or "L;", or beginning with "ONCLICK:" or "TOOLTIP:".
bool isKnownTagWord(const std::vector<CodePoint>& v, std::size_t from, std::size_t to) {
    const std::size_t length = to - from;
    constexpr CodePoint kL = 0x4C;
    constexpr CodePoint kSemicolon = 0x3B;
    if (length == 1 && v[from] == kL) {
        return true;
    }
    if (length == 2 && v[from] == kL && v[from + 1] == kSemicolon) {
        return true;
    }
    return startsWith(v, from, to, "ONCLICK:") || startsWith(v, from, to, "TOOLTIP:");
}

bool isRemovedCodePoint(CodePoint cp) {
    return cp < 0x20U || cp == 0x7FU || (cp >= 0x80U && cp <= 0x9FU) ||
           (cp >= 0x2028U && cp <= 0x202EU) || (cp >= 0x2066U && cp <= 0x2069U);
}

}  // namespace

std::string repairUtf8(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (const CodePoint cp : decode(raw)) {
        appendUtf8(out, cp);
    }
    return out;
}

std::string cleanText(std::string_view raw) {
    // Step 1.
    const std::vector<CodePoint> decoded = decode(raw);

    // Step 2: one left-to-right pass deleting the pair 0x15 0x21.
    std::vector<CodePoint> noPairs;
    noPairs.reserve(decoded.size());
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        if (decoded[i] == kLinkMarker && i + 1 < decoded.size() && decoded[i + 1] == kLinkEnd) {
            ++i;
            continue;
        }
        noPairs.push_back(decoded[i]);
    }

    // Step 3: a 0x15 followed by a known tag word is removed with the word and one space. A 0x15
    // whose word is unknown stays for step 4; scanning goes on right after it.
    std::vector<CodePoint> noTags;
    noTags.reserve(noPairs.size());
    // The end of the last space-free run found. A later 0x15 inside that run has the same word
    // end, so a run of 0x15 bytes is scanned once, not once per byte (which would be quadratic).
    std::size_t runEnd = 0;
    for (std::size_t i = 0; i < noPairs.size();) {
        if (noPairs[i] != kLinkMarker) {
            noTags.push_back(noPairs[i]);
            ++i;
            continue;
        }
        std::size_t wordEnd = i + 1;
        if (wordEnd <= runEnd) {
            wordEnd = runEnd;
        } else {
            while (wordEnd < noPairs.size() && noPairs[wordEnd] != kSpace) {
                ++wordEnd;
            }
            runEnd = wordEnd;
        }
        if (isKnownTagWord(noPairs, i + 1, wordEnd)) {
            i = wordEnd < noPairs.size() ? wordEnd + 1 : wordEnd;
            continue;
        }
        noTags.push_back(noPairs[i]);
        ++i;
    }

    // Steps 4 and 5 in one pass: drop removed code points, collapse runs of U+0020, trim.
    std::string out;
    out.reserve(raw.size());
    bool pendingSpace = false;
    for (const CodePoint cp : noTags) {
        if (isRemovedCodePoint(cp)) {
            continue;
        }
        if (cp == kSpace) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        appendUtf8(out, cp);
    }
    return out;
}

}  // namespace curia::bridge
