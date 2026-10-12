// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace curia {

// The stdin language of `curia --headless-chat`, one command per line:
//
//   ask <text>          ask the advisor; the text is the rest of the line
//   cancel              stop the answer in flight (the text received so far is kept)
//   arm                 the overlay was opened by the player (hot key or tray): arms the
//                       log-silence detector
//   refresh             the player pressed Refresh in the overlay (same effect as arm)
//   retry               answer the last question again
//   ck3 running|stopped tell the app whether the game is running (the headless mode has no
//                       process probe of its own; the tests and the owner say so here)
//   quit                stop now (an answer in flight is cancelled)
//
// The verb is case-insensitive, blanks around it are ignored, a trailing CR is ignored, an empty
// line is nothing. The end of the input is "quit when the work is done".
struct HeadlessCommand {
    enum class Kind {
        // An empty line.
        Nothing,
        Ask,
        Cancel,
        Arm,
        Refresh,
        Retry,
        Ck3,
        Quit,
        // Not a command; `problem` says why.
        Invalid,
    };
    Kind kind = Kind::Nothing;
    // Ask: the question.
    std::string text;
    // Ck3: the state told.
    bool ck3Running = true;
    // Invalid: a short reason (never repeats the line, which could hold a secret).
    std::string problem;
};

// A line longer than this is refused (Invalid) instead of being parsed.
inline constexpr std::size_t kMaxCommandLineBytes = 64 * 1024;

HeadlessCommand parseHeadlessCommand(std::string_view line);

}  // namespace curia
