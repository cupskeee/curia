// SPDX-License-Identifier: MIT
#include "advisor/plain_words.h"

#include <cstddef>

namespace curia::advisor {

namespace {

char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool matchesAt(std::string_view text, std::size_t at, std::string_view word) {
    if (at + word.size() > text.size()) {
        return false;
    }
    for (std::size_t k = 0; k < word.size(); ++k) {
        if (lowerAscii(text[at + k]) != word[k]) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string_view findInternalWord(std::string_view text) {
    for (const InternalWord& entry : kInternalWords) {
        for (std::size_t at = 0; at < text.size(); ++at) {
            if (!matchesAt(text, at, entry.word)) {
                continue;
            }
            const std::size_t end = at + entry.word.size();
            const bool alone = (at == 0 || !isWordChar(text[at - 1])) &&
                               (end == text.size() || !isWordChar(text[end]));
            if (!entry.wholeWord || alone) {
                return entry.word;
            }
        }
    }
    return {};
}

}  // namespace curia::advisor
