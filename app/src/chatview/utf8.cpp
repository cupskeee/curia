// SPDX-License-Identifier: MIT
#include "chatview/utf8.h"

namespace curia::chatview {

namespace {

bool isContinuation(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

// The length of the valid sequence that starts at `at`, or 0 if none does. Overlong forms,
// surrogates and values above U+10FFFF are not valid.
std::size_t sequenceLength(std::string_view text, std::size_t at, char32_t& value) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = 0;
    char32_t cp = 0;
    char32_t minimum = 0;
    if (lead < 0x80U) {
        value = lead;
        return 1;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        cp = lead & 0x1FU;
        minimum = 0x80;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        cp = lead & 0x0FU;
        minimum = 0x800;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        cp = lead & 0x07U;
        minimum = 0x10000;
    } else {
        return 0;
    }
    if (at + length > text.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const auto byte = static_cast<unsigned char>(text[at + k]);
        if (!isContinuation(byte)) {
            return 0;
        }
        cp = (cp << 6U) | (byte & 0x3FU);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    value = cp;
    return length;
}

}  // namespace

std::size_t nextCodePoint(std::string_view text, std::size_t at) {
    char32_t value = 0;
    const std::size_t length = sequenceLength(text, at, value);
    return at + (length == 0 ? 1 : length);
}

std::vector<char32_t> decodeUtf8(std::string_view text) {
    std::vector<char32_t> out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        char32_t value = 0;
        const std::size_t length = sequenceLength(text, at, value);
        out.push_back(length == 0 ? kReplacementCharacter : value);
        at += length == 0 ? 1 : length;
    }
    return out;
}

}  // namespace curia::chatview
