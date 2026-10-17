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
//     claims as a count only, income "about +-0.2", "not shown" for an absent section);
//   * plain words only (advisor/plain_words.h): the model copies the block's wording, so the
//     block never says "snapshot", "export", "data" and the like, which a player would not
//     understand (owner rule, 2026-10-17). Its section name, WHAT THE ADVISOR CAN SEE, is the one
//     the persona refers to;
//   * every game string (names, titles, war names) goes through sanitizeGameText(): quoted,
//     control characters, angle brackets and markers removed, at most 64 characters.
namespace curia::advisor {

inline constexpr std::string_view kBeginMarker = "=== WHAT THE ADVISOR CAN SEE ===";
inline constexpr std::string_view kEndMarker = "=== END OF WHAT THE ADVISOR CAN SEE ===";

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
//   * replaces the marker phrase "advisor can see" (any letter case) with "advisor can_see", and
//     the words "game data" (the old marker) with "game_data", after these lookalikes are read as
//     the real characters: fullwidth ASCII as ASCII, lookalikes of '=' as '=', other Unicode
//     spaces and the braille blank as a blank, invisible characters (every format character and
//     the fillers) dropped. So a name cannot draw the end of the block with them. Letters that
//     only look like Latin ones (Cyrillic, Greek, mathematical or circled forms) are not mapped;
//     the quotes and the '=' rule still keep such a name from starting a marker line;
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

// The block used while nothing has been read from the game yet (no Complete frame).
std::string renderNoSnapshot();

}  // namespace curia::advisor
