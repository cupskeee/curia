// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <vector>

// How an answer is shown (FR-OVL-12, owner, 2026-10-17): **bold** is rendered, every other piece of
// markdown is removed, and lines that start with a number ("1. ") stay as they are, because the
// persona asks the model for a numbered list of actions. Pure text processing, no UI.
namespace curia::chatview {

struct Run {
    std::string text;
    bool bold = false;

    friend bool operator==(const Run&, const Run&) = default;
};

// One line of an answer. `marker` is the start of a list line ("• " for a bullet, "1. " for a
// numbered line, after the indent of a nested item), drawn before the runs; the lines a long list
// line wraps into are indented by its width. A line without marker and runs is a blank line
// between paragraphs.
struct AnswerLine {
    std::string marker;
    std::vector<Run> runs;

    [[nodiscard]] bool blank() const { return marker.empty() && runs.empty(); }
    friend bool operator==(const AnswerLine&, const AnswerLine&) = default;
};

struct AnswerText {
    std::vector<AnswerLine> lines;
};

// The bullet a "- " or "* " list line gets (U+2022 and a space; the bundled font has it).
inline constexpr std::string_view kBulletMarker = "\xE2\x80\xA2 ";

// Parses an answer, finished or still arriving (`complete` false: the text may end in the middle of
// a marker). The rules:
//   * "**x**" and "__x__" are bold;
//   * "*x*" and "_x_" lose their markers when they clearly wrap a word (a marker inside a word,
//     as in snake_case or a name, and a lone "*" between blanks are text); "~~x~~" (strikethrough)
//     loses its markers and x stays plain, an unmatched "~~" is dropped, a single "~" is text;
//   * "# " to "###### " at the start of a line and "> " quote markers are removed (the line stays,
//     plain); "- " and "* " bullets become kBulletMarker; "1. " and "1) " numbered lines are kept
//     exactly; backticks of code are removed (the code is shown as plain text, its content
//     untouched); a link [text](url) becomes its text; a line of three or more "-", "*", "_" or
//     "=" (a rule) is dropped, and so are code fence lines; tables stay as text; a backslash before
//     punctuation shows the punctuation;
//   * runs of blank lines become one blank line, and blank lines at the start and the end go;
//   * while the answer arrives, nothing flickers: a marker at the very end of the text is not shown
//     (it may be the start of "**"), the text after an opening "**" is bold until it closes, and a
//     last line that holds markers only is not shown yet. Once the answer is complete an unmatched
//     "**" is dropped and its text is plain.
// Only the last line of an unfinished answer counts as unfinished: a line before it is complete.
// UTF-8 safe: only ASCII bytes are ever markers, so no multi-byte character is split.
AnswerText parseAnswer(std::string_view text, bool complete);

// One plain line per line of `text` (no markup), blank lines kept: how a question is shown.
AnswerText plainLines(std::string_view text);

// What the Copy button puts on the clipboard: the shown text without markers, one line per line
// ("\n" between them), list markers included.
std::string plainText(const AnswerText& answer);

}  // namespace curia::chatview
