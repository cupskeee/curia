// SPDX-License-Identifier: MIT
#include "chatview/text_layout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "chatview/utf8.h"

namespace curia::chatview {

namespace {

bool isBlank(char c) {
    return c == ' ' || c == '\t';
}

// A word: the characters between two blanks, possibly plain and bold in turn.
struct Word {
    std::vector<Run> pieces;
};

// A word and the blanks before it.
struct Token {
    std::string blanks;
    bool blanksBold = false;
    Word word;
};

// Splits the runs of one line into words and the blanks between them. Tabs become four blanks.
std::vector<Token> tokenize(const std::vector<Run>& runs) {
    std::vector<Token> tokens;
    Token current;
    for (const Run& run : runs) {
        const std::string& text = run.text;
        std::size_t i = 0;
        while (i < text.size()) {
            std::size_t j = i;
            if (isBlank(text[i])) {
                while (j < text.size() && isBlank(text[j])) {
                    ++j;
                }
                if (!current.word.pieces.empty()) {
                    tokens.push_back(std::move(current));
                    current = Token{};
                }
                if (current.blanks.empty()) {
                    current.blanksBold = run.bold;
                }
                for (std::size_t k = i; k < j; ++k) {
                    current.blanks += text[k] == '\t' ? std::string(4, ' ') : std::string(1, ' ');
                }
            } else {
                while (j < text.size() && !isBlank(text[j])) {
                    ++j;
                }
                std::vector<Run>& pieces = current.word.pieces;
                if (!pieces.empty() && pieces.back().bold == run.bold) {
                    pieces.back().text.append(text, i, j - i);
                } else {
                    pieces.push_back(Run{text.substr(i, j - i), run.bold});
                }
            }
            i = j;
        }
    }
    if (!current.word.pieces.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

class Builder {
public:
    Builder(float maxWidth, const MeasureFn& measure, TextLayout& out)
        : maxWidth_(maxWidth), measure_(measure), out_(out) {}

    void addLine(const AnswerLine& line) {
        if (line.blank()) {
            out_.lines.emplace_back();
            return;
        }
        current_ = LayoutLine{};
        x_ = 0;
        indent_ = 0;
        hasWord_ = false;
        if (!line.marker.empty()) {
            const float markerWidth = measure_(line.marker, false);
            append(line.marker, false, markerWidth);
            indent_ = markerWidth <= maxWidth_ * 0.5F ? markerWidth : 0.0F;
        }
        for (const Token& token : tokenize(line.runs)) {
            const float wordWidth = widthOf(token.word);
            if (!hasWord_) {
                // The first word of the line (place() always leaves a word on the line).
                if (!token.blanks.empty()) {
                    // An indent at the start of a text line is kept while it leaves room.
                    const float blanksWidth = measure_(token.blanks, token.blanksBold);
                    if (x_ + blanksWidth < maxWidth_) {
                        append(token.blanks, token.blanksBold, blanksWidth);
                    }
                }
                place(token.word, wordWidth);
                continue;
            }
            const float blanksWidth = measure_(token.blanks, token.blanksBold);
            if (x_ + blanksWidth + wordWidth <= maxWidth_) {
                append(token.blanks, token.blanksBold, blanksWidth);
                place(token.word, wordWidth);
            } else {
                finishLine();
                place(token.word, wordWidth);
            }
        }
        finishLine();
    }

private:
    [[nodiscard]] float widthOf(const Word& word) const {
        float width = 0;
        for (const Run& piece : word.pieces) {
            width += measure_(piece.text, piece.bold);
        }
        return width;
    }

    void append(std::string_view text, bool bold, float width) {
        std::vector<LayoutSegment>& segments = current_.segments;
        if (!segments.empty() && segments.back().bold == bold &&
            std::fabs(segments.back().x + segments.back().width - x_) < 0.001F) {
            segments.back().text += text;
            segments.back().width += width;
        } else {
            segments.push_back(LayoutSegment{std::string(text), bold, x_, width});
        }
        x_ += width;
    }

    void finishLine() {
        float width = 0;
        if (!current_.segments.empty()) {
            const LayoutSegment& last = current_.segments.back();
            width = last.x + last.width;
        }
        out_.width = std::max(out_.width, width);
        out_.lines.push_back(std::move(current_));
        current_ = LayoutLine{};
        x_ = indent_;
        hasWord_ = false;
    }

    // Puts a word at the current position; one that does not fit on a line of its own is broken
    // between characters (the caller has already moved to a new line if the word fits there).
    void place(const Word& word, float width) {
        if (x_ + width <= maxWidth_) {
            for (const Run& piece : word.pieces) {
                append(piece.text, piece.bold, measure_(piece.text, piece.bold));
            }
            hasWord_ = true;
            return;
        }
        for (const Run& piece : word.pieces) {
            const std::string_view text = piece.text;
            std::size_t at = 0;
            while (at < text.size()) {
                const std::size_t next = nextCodePoint(text, at);
                const std::string_view character = text.substr(at, next - at);
                const float characterWidth = measure_(character, piece.bold);
                if (hasWord_ && x_ + characterWidth > maxWidth_) {
                    finishLine();
                }
                append(character, piece.bold, characterWidth);
                hasWord_ = true;
                at = next;
            }
        }
    }

    float maxWidth_;
    const MeasureFn& measure_;
    TextLayout& out_;
    LayoutLine current_;
    float x_ = 0;
    float indent_ = 0;
    bool hasWord_ = false;
};

}  // namespace

TextLayout layoutText(const AnswerText& text, float maxWidth, const MeasureFn& measure) {
    TextLayout out;
    Builder builder(maxWidth, measure, out);
    for (const AnswerLine& line : text.lines) {
        builder.addLine(line);
    }
    return out;
}

}  // namespace curia::chatview
