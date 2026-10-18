// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "chatview/answer_text.h"

// Word wrapping of an answer (runs of plain and bold text) into lines no wider than a given width.
// The width of a piece of text comes from an injected function: the chat window passes the font's
// own measurement, the tests a fixed width per character. Pure and deterministic.
namespace curia::chatview {

// The width of `text` in the regular or the bold face, in the caller's unit (pixels). The layout
// adds the widths of pieces, so the function must be additive (no kerning across pieces).
using MeasureFn = std::function<float(std::string_view text, bool bold)>;

struct LayoutSegment {
    std::string text;
    bool bold = false;
    // Where the segment starts, from the left edge of the text.
    float x = 0;
    float width = 0;
};

// One line on screen. No segments = an empty line (a blank line of the text).
struct LayoutLine {
    std::vector<LayoutSegment> segments;
};

struct TextLayout {
    std::vector<LayoutLine> lines;
    // The widest line.
    float width = 0;
};

// The rules:
//   * every AnswerLine starts a new line; a blank one is an empty line;
//   * lines break at blanks; the blank at a break is not drawn, and neither are blanks at the end
//     of a line; blanks at the start of a text line (an indent the player typed) are kept;
//   * a word wider than the whole line is broken between two characters (code points, never inside
//     a UTF-8 sequence), at least one character per line so the layout always ends;
//   * a word may change between plain and bold in the middle ("**Pay**s"): it stays one word;
//   * the continuation lines of a list line start at the width of its marker (a hanging indent),
//     unless the marker takes more than half of the width;
//   * a width of 0 or less is treated as the width of one character: every character on a line.
TextLayout layoutText(const AnswerText& text, float maxWidth, const MeasureFn& measure);

}  // namespace curia::chatview
