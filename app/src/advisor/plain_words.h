// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <string_view>

// The plain-words rule (owner, 2026-10-17; docs/requirements.md FR-OVL-11): nothing the player
// reads uses Curia's internal words. That covers the chat window's own texts (notices, status
// lines, errors, dividers) and, because the model copies the words it is given, the persona, the
// block the model reads and the freshness note as well. The unit tests check those texts against
// this list, and the chat window's texts are to be checked against it too: one list, here.
namespace curia::advisor {

struct InternalWord {
    // Lower-case ASCII.
    std::string_view word;
    // Only as a whole word ("data", but not inside "update"); otherwise anywhere, so that every
    // form is caught ("exported", "snapshots", "debug.log").
    bool wholeWord = false;
};

inline constexpr std::array<InternalWord, 21> kInternalWords = {{
    {"snapshot", false},
    {"snap", true},
    {"export", false},
    {"game data", false},
    {"data", true},
    {"frame", false},
    {"debug", false},
    {"curia1", false},
    {"log", true},
    {"logs", true},
    {"logged", true},
    {"logging", true},
    {"marker", false},
    {"json", false},
    {"token", false},
    // Ordinary words, but Curia's names for parts of its own work ("the block the model reads",
    // "a FIN record", "a field"): whole words only, so "blocked" or "battlefield" stay allowed.
    {"block", true},
    {"blocks", true},
    {"record", true},
    {"records", true},
    {"field", true},
    {"fields", true},
}};

// The first internal word found in `text` (ASCII letter case ignored), or an empty view.
std::string_view findInternalWord(std::string_view text);

}  // namespace curia::advisor
