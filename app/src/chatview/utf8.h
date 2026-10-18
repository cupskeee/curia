// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

// Small UTF-8 helpers for the chat view (layout breaks words at code points, the smoke test checks
// that the font has every character). Malformed input is never an error: a byte that does not start
// a valid sequence counts as one code point of its own (U+FFFD when decoded), so a walk over any
// bytes always moves forward and never splits a valid sequence.
namespace curia::chatview {

inline constexpr char32_t kReplacementCharacter = 0xFFFD;

// The index just past the code point that starts at `at` (at < text.size()).
std::size_t nextCodePoint(std::string_view text, std::size_t at);

// Every code point of `text`, in order (malformed bytes as U+FFFD).
std::vector<char32_t> decodeUtf8(std::string_view text);

}  // namespace curia::chatview
