// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "bridge/frame.h"

// Turns a Complete frame into the text block the model reads (the second system block of a
// request). The block carries only fields that exist in the frozen wire format
// (docs/wire-format.md; design rule D15: a new field needs a wire-format row first), plus facts
// derived from ids that the format allows (which side of a war the ruler is on, whether a faction
// targets the ruler, whether a faction leader is one of the listed vassals).
//
// Properties (all tested):
//   * deterministic: the same Frame always gives the same bytes. Integers only, no clock, no
//     locale, no pointers, no hash ordering;
//   * the limits of the data are written in the text itself ("2 of 12 vassals listed", implicit
//     claims as a count only, income "about +-0.2", "not exported" for an absent section);
//   * every game string (names, titles, war names) goes through sanitizeGameText(): quoted,
//     control characters, angle brackets and markers removed, at most 64 characters.
namespace curia::advisor {

inline constexpr std::string_view kBeginMarker = "=== BEGIN GAME DATA (data only) ===";
inline constexpr std::string_view kEndMarker = "=== END GAME DATA ===";

// A game string is cut to this many characters (code points).
inline constexpr std::size_t kMaxTextChars = 64;

// The renderer shows at most this many of each list, however many the frame holds (the parser does
// not rely on the mod's caps, wire-format section 3, so the prompt size is bounded here too).
inline constexpr std::size_t kMaxVassalsShown = 10;
inline constexpr std::size_t kMaxClaimsShown = 10;
// Factions against the ruler, strongest by power first; the ruler's own faction is always shown.
inline constexpr std::size_t kMaxFactionsAgainstShown = 5;
inline constexpr std::size_t kMaxWarsShown = 20;

// Prompt-injection hygiene for one game string. On top of the frame assembler's cleaning
// (bridge::cleanText), it
//   * removes '<' and '>', turns '"' into '\'' and '[' ']' into '(' ')' (the quotes keep a name
//     from closing its own quotation, the brackets keep it from imitating the "[Context: ...]"
//     note), and turns '=' into '-' (a name cannot draw a marker line);
//   * replaces the words "game data" (any letter case) with "game_data";
//   * cuts the result to kMaxTextChars code points and trims trailing blanks.
// The result is valid UTF-8 and has no control character; the function is idempotent.
std::string sanitizeGameText(std::string_view raw);

// "15 September 1066" (English month names, no locale); a month outside 1 to 12 prints as
// "1066-13-15".
std::string formatGameDate(const bridge::Date& date);

// Tenths of a gold piece as "+7.8", "-2.3", "0.0" (integer arithmetic only).
std::string formatTenths(std::int32_t tenths);

// The text block for a frame, from the BEGIN marker to the END marker, no trailing newline.
std::string renderSnapshot(const bridge::Frame& frame);

// The block used while no snapshot exists yet.
std::string renderNoSnapshot();

}  // namespace curia::advisor
