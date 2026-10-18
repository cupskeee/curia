// SPDX-License-Identifier: MIT
#include "chatview/answer_text.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace curia::chatview {

namespace {

bool isBlank(char c) {
    return c == ' ' || c == '\t';
}

bool isAsciiPunct(char c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') ||
           (c >= '{' && c <= '~');
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// A letter or digit, counting every byte of a multi-byte character as one: "Ælfwynn_Test" is one
// word to the marker rules.
bool isWordByte(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte >= 0x80U || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c);
}

std::string_view trimLeft(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() && isBlank(text[start])) {
        ++start;
    }
    return text.substr(start);
}

std::string_view trimRight(std::string_view text) {
    std::size_t end = text.size();
    while (end > 0 && isBlank(text[end - 1])) {
        --end;
    }
    return text.substr(0, end);
}

std::string_view trim(std::string_view text) {
    return trimRight(trimLeft(text));
}

// The columns of the leading blanks (a tab counts 4).
std::size_t leadingColumns(std::string_view text) {
    std::size_t columns = 0;
    for (const char c : text) {
        if (c == ' ') {
            ++columns;
        } else if (c == '\t') {
            columns += 4;
        } else {
            break;
        }
    }
    return columns;
}

// "---", "***", "___", "===" and longer, also with blanks between ("* * *").
bool isRule(std::string_view trimmed) {
    if (trimmed.empty()) {
        return false;
    }
    const char c = trimmed.front();
    if (c != '-' && c != '*' && c != '_' && c != '=') {
        return false;
    }
    std::size_t count = 0;
    for (const char ch : trimmed) {
        if (ch == c) {
            ++count;
        } else if (!isBlank(ch)) {
            return false;
        }
    }
    return count >= 3;
}

// "```" or "~~~", with an optional language word after it.
bool isFence(std::string_view trimmed) {
    return trimmed.size() >= 3 && (trimmed.substr(0, 3) == "```" || trimmed.substr(0, 3) == "~~~");
}

// A line made of marker characters only: while it is the last line of an unfinished answer it may
// still become a rule, a heading, a bullet, a quote or a fence, so it is not shown yet.
bool isMarkersOnly(std::string_view trimmed) {
    return !trimmed.empty() && std::all_of(trimmed.begin(), trimmed.end(), [](char c) {
        return c == '#' || c == '-' || c == '*' || c == '_' || c == '=' || c == '>' || c == '`' ||
               c == '~' || isBlank(c);
    });
}

// The indent of a nested list item: none, or one or two steps of three blanks.
std::string nestIndent(std::size_t columns) {
    if (columns < 2) {
        return {};
    }
    return columns < 6 ? std::string(3, ' ') : std::string(6, ' ');
}

// ---- inline markup -----------------------------------------------------------------------------

enum class Role : std::uint8_t {
    // Shown as its characters.
    Literal,
    // Opens a span that a later delimiter closes.
    Open,
    Close,
    // Opens a bold span that never closes while the line is still arriving: bold to the end.
    OpenUnclosed,
    // Not shown and no effect (a marker at the end of an unfinished line, an unmatched "**" of a
    // complete line, an unmatched "*" of an unfinished line).
    Hidden,
};

struct Piece {
    bool delimiter = false;
    std::string text;  // a text piece's characters, or a delimiter's marker characters
    int count = 0;     // a delimiter's length: 1 (emphasis) or 2 (bold, or "~~" strikethrough)
    char ch = 0;       // '*', '_' or '~'
    bool canOpen = false;
    bool canClose = false;
    Role role = Role::Literal;
};

class InlineParser {
public:
    InlineParser(std::string_view text, bool lineComplete) : text_(text), complete_(lineComplete) {}

    std::vector<Run> parse() {
        tokenize();
        match();
        return emit();
    }

private:
    void flushText() {
        if (!buffer_.empty()) {
            Piece piece;
            piece.text = std::move(buffer_);
            pieces_.push_back(std::move(piece));
            buffer_.clear();
        }
    }

    void addText(std::string_view text) {
        flushText();
        Piece piece;
        piece.text = std::string(text);
        pieces_.push_back(std::move(piece));
    }

    [[nodiscard]] std::optional<char> charAt(std::size_t at) const {
        if (at >= text_.size()) {
            return std::nullopt;
        }
        return text_[at];
    }

    // The delimiter run of `count` characters at `at` (CommonMark's flanking rules, plus: a single
    // marker must not touch a letter on its outer side, so that it only wraps whole words).
    void addDelimiterRun(std::size_t at, std::size_t count) {
        const char ch = text_[at];
        const std::optional<char> before = at == 0 ? std::nullopt : charAt(at - 1);
        const std::optional<char> after = charAt(at + count);
        const bool beforeSpace = !before.has_value() || isBlank(*before);
        const bool afterSpace = !after.has_value() || isBlank(*after);
        const bool beforePunct = before.has_value() && isAsciiPunct(*before);
        const bool afterPunct = after.has_value() && isAsciiPunct(*after);
        const bool left = !afterSpace && (!afterPunct || beforeSpace || beforePunct);
        const bool right = !beforeSpace && (!beforePunct || afterSpace || afterPunct);
        bool canOpen = left;
        bool canClose = right;
        if (ch == '_') {
            canOpen = left && (!right || beforePunct);
            canClose = right && (!left || afterPunct);
        }
        flushText();
        const bool trailing = !complete_ && at + count == text_.size();
        if (count > 3 || trailing) {
            Piece piece;
            piece.delimiter = true;
            piece.text = std::string(count, ch);
            piece.role = trailing ? Role::Hidden : Role::Literal;
            pieces_.push_back(std::move(piece));
            return;
        }
        auto add = [&](int length) {
            Piece piece;
            piece.delimiter = true;
            piece.text = std::string(static_cast<std::size_t>(length), ch);
            piece.count = length;
            piece.ch = ch;
            piece.canOpen = canOpen;
            piece.canClose = canClose;
            if (length == 1) {
                const bool wordBefore = before.has_value() && isWordByte(*before);
                const bool wordAfter = after.has_value() && isWordByte(*after);
                piece.canOpen = piece.canOpen && !wordBefore;
                piece.canClose = piece.canClose && !wordAfter;
            }
            pieces_.push_back(std::move(piece));
        };
        if (count == 3) {
            // "***x***": bold outside, emphasis inside.
            if (canOpen && !canClose) {
                add(2);
                add(1);
            } else {
                add(1);
                add(2);
            }
        } else {
            add(static_cast<int>(count));
        }
    }

    // A link "[text](url)" or "![text](url)" at `at` ('[' there): the text is parsed like the rest
    // of the line, the brackets and the address are dropped. Returns false if there is none.
    bool startLink(std::size_t at) {
        const std::size_t close = text_.find(']', at + 1);
        if (close == std::string_view::npos) {
            // "[Vassals" still arriving: show the text, keep the bracket back.
            return !complete_;
        }
        if (close + 1 >= text_.size()) {
            // "[text]" at the end of an unfinished line: a "(" may follow.
            if (!complete_) {
                skipFrom_ = close;
                skipTo_ = close + 1;
                return true;
            }
            return false;
        }
        if (text_[close + 1] != '(') {
            return false;
        }
        const std::size_t end = text_.find(')', close + 2);
        if (end == std::string_view::npos) {
            if (!complete_) {
                skipFrom_ = close;
                skipTo_ = text_.size();
                return true;
            }
            return false;
        }
        skipFrom_ = close;
        skipTo_ = end + 1;
        return true;
    }

    void tokenize() {
        std::size_t i = 0;
        while (i < text_.size()) {
            if (skipFrom_.has_value() && i >= *skipFrom_) {
                // The end of a link: its "](address)" is not shown. (A code span that ran over the
                // bracket ends the link where it is.)
                if (i == *skipFrom_) {
                    i = skipTo_;
                }
                skipFrom_.reset();
                continue;
            }
            const char c = text_[i];
            if (c == '\\') {
                if (i + 1 < text_.size() && isAsciiPunct(text_[i + 1])) {
                    buffer_ += text_[i + 1];
                    i += 2;
                    continue;
                }
                if (i + 1 == text_.size() && !complete_) {
                    ++i;  // an escape that is still arriving
                    continue;
                }
                buffer_ += c;
                ++i;
                continue;
            }
            if (c == '`') {
                std::size_t run = 0;
                while (i + run < text_.size() && text_[i + run] == '`') {
                    ++run;
                }
                const std::size_t contentStart = i + run;
                const std::string_view fence = text_.substr(i, run);
                std::size_t search = contentStart;
                std::size_t closeAt = std::string_view::npos;
                while (search < text_.size()) {
                    const std::size_t found = text_.find(fence, search);
                    if (found == std::string_view::npos) {
                        break;
                    }
                    std::size_t length = 0;
                    while (found + length < text_.size() && text_[found + length] == '`') {
                        ++length;
                    }
                    if (length == run) {
                        closeAt = found;
                        break;
                    }
                    search = found + length;
                }
                if (closeAt != std::string_view::npos) {
                    // Code: its content is shown as it is, markers and all.
                    addText(text_.substr(contentStart, closeAt - contentStart));
                    i = closeAt + run;
                } else {
                    i = contentStart;  // an unmatched backtick is dropped
                }
                continue;
            }
            if (c == '[' || (c == '!' && i + 1 < text_.size() && text_[i + 1] == '[' &&
                             !skipFrom_.has_value())) {
                const std::size_t bracket = c == '[' ? i : i + 1;
                if (!skipFrom_.has_value() && startLink(bracket)) {
                    i = bracket + 1;
                    continue;
                }
                buffer_ += c;
                ++i;
                continue;
            }
            if (c == '*' || c == '_' || c == '~') {
                std::size_t run = 0;
                while (i + run < text_.size() && text_[i + run] == c) {
                    ++run;
                }
                // A "~" at the very end of an unfinished line may become "~~": held back below.
                const bool mayGrow = run == 1 && !complete_ && i + 1 == text_.size();
                if (c == '~' && run != 2 && !mayGrow) {
                    // Only "~~" marks strikethrough; "~100 gold" and "~~~" are text.
                    buffer_.append(run, c);
                } else {
                    addDelimiterRun(i, run);
                }
                i += run;
                continue;
            }
            buffer_ += c;
            ++i;
        }
        flushText();
    }

    void match() {
        std::vector<std::size_t> openers;
        for (std::size_t k = 0; k < pieces_.size(); ++k) {
            Piece& piece = pieces_[k];
            if (!piece.delimiter || piece.count == 0 || piece.role == Role::Hidden) {
                continue;
            }
            bool closed = false;
            if (piece.canClose) {
                for (std::size_t s = openers.size(); s > 0; --s) {
                    Piece& opener = pieces_[openers[s - 1]];
                    if (opener.ch == piece.ch && opener.count == piece.count) {
                        opener.role = Role::Open;
                        piece.role = Role::Close;
                        // Openers inside the matched pair that found no partner stay unmatched.
                        for (std::size_t t = s; t < openers.size(); ++t) {
                            unmatched(pieces_[openers[t]]);
                        }
                        openers.resize(s - 1);
                        closed = true;
                        break;
                    }
                }
            }
            if (closed) {
                continue;
            }
            if (piece.canOpen) {
                openers.push_back(k);
            } else if (piece.count == 2 && piece.canClose) {
                // A closing "**" with nothing to close: dropped, like an unmatched opening one.
                piece.role = Role::Hidden;
            } else {
                piece.role = Role::Literal;
            }
        }
        for (const std::size_t k : openers) {
            unmatched(pieces_[k]);
        }
    }

    void unmatched(Piece& piece) const {
        if (piece.count == 2) {
            piece.role = complete_ ? Role::Hidden : Role::OpenUnclosed;
        } else {
            // A lone "*" or "_" is text, but only once the line is complete: until then its
            // partner may still come, and a marker that appears and then goes would flicker.
            piece.role = complete_ ? Role::Literal : Role::Hidden;
        }
    }

    [[nodiscard]] std::vector<Run> emit() const {
        std::vector<Run> runs;
        int bold = 0;
        auto append = [&](const std::string& text) {
            if (text.empty()) {
                return;
            }
            const bool isBold = bold > 0;
            if (!runs.empty() && runs.back().bold == isBold) {
                runs.back().text += text;
            } else {
                runs.push_back(Run{text, isBold});
            }
        };
        for (const Piece& piece : pieces_) {
            if (!piece.delimiter) {
                append(piece.text);
                continue;
            }
            switch (piece.role) {
            case Role::Literal:
                append(piece.text);
                break;
            case Role::Open:
            case Role::OpenUnclosed:
                // "~~x~~" (strikethrough) loses its markers and changes nothing.
                if (piece.count == 2 && piece.ch != '~') {
                    ++bold;
                }
                break;
            case Role::Close:
                if (piece.count == 2 && piece.ch != '~' && bold > 0) {
                    --bold;
                }
                break;
            case Role::Hidden:
                break;
            }
        }
        // Blanks at the end of a line are not shown (a marker after them may have been dropped).
        while (!runs.empty()) {
            std::string& last = runs.back().text;
            while (!last.empty() && isBlank(last.back())) {
                last.pop_back();
            }
            if (!last.empty()) {
                break;
            }
            runs.pop_back();
        }
        return runs;
    }

    std::string_view text_;
    bool complete_;
    std::vector<Piece> pieces_;
    std::string buffer_;
    std::optional<std::size_t> skipFrom_;
    std::size_t skipTo_ = 0;
};

// ---- lines --------------------------------------------------------------------------------------

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find('\n', start);
        std::string_view line = text.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

void addBlank(std::vector<AnswerLine>& lines) {
    if (!lines.empty() && !lines.back().blank()) {
        lines.emplace_back();
    }
}

// The marker of a list line and the text after it; nullopt for a line that is no list item.
struct ListItem {
    std::string marker;
    std::string_view rest;
};

std::optional<ListItem> listItem(std::string_view rest, std::size_t columns) {
    if (rest.size() >= 2 && (rest[0] == '-' || rest[0] == '*') && isBlank(rest[1])) {
        return ListItem{nestIndent(columns) + std::string(kBulletMarker), trimLeft(rest.substr(2))};
    }
    // A bullet the model wrote itself.
    const std::string_view bullet = kBulletMarker.substr(0, kBulletMarker.size() - 1);
    if (rest.size() > bullet.size() && rest.substr(0, bullet.size()) == bullet &&
        isBlank(rest[bullet.size()])) {
        return ListItem{nestIndent(columns) + std::string(kBulletMarker),
                        trimLeft(rest.substr(bullet.size() + 1))};
    }
    std::size_t digits = 0;
    while (digits < rest.size() && digits < 4 && isDigit(rest[digits])) {
        ++digits;
    }
    if (digits >= 1 && digits <= 3 && digits + 1 < rest.size() &&
        (rest[digits] == '.' || rest[digits] == ')') && isBlank(rest[digits + 1])) {
        // Kept exactly: "1. " stays "1. ".
        return ListItem{nestIndent(columns) + std::string(rest.substr(0, digits + 1)) + " ",
                        trimLeft(rest.substr(digits + 2))};
    }
    return std::nullopt;
}

}  // namespace

AnswerText parseAnswer(std::string_view text, bool complete) {
    AnswerText answer;
    std::vector<AnswerLine>& out = answer.lines;
    const std::vector<std::string_view> lines = splitLines(text);
    bool inCode = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const bool lineComplete = complete || i + 1 < lines.size();
        const std::string_view line = lines[i];
        const std::string_view trimmed = trim(line);
        if (isFence(trimmed) && (lineComplete || trimmed.size() > 3)) {
            inCode = !inCode;
            continue;
        }
        if (inCode) {
            // Inside a code block: shown as it is.
            const std::string_view code = trimRight(line);
            if (code.empty()) {
                addBlank(out);
            } else {
                AnswerLine plain;
                plain.runs.push_back(Run{std::string(code), false});
                out.push_back(std::move(plain));
            }
            continue;
        }
        if (trimmed.empty()) {
            addBlank(out);
            continue;
        }
        if (!lineComplete && isMarkersOnly(trimmed)) {
            continue;
        }
        if (isRule(trimmed)) {
            continue;
        }

        std::size_t columns = leadingColumns(line);
        std::string_view rest = trimmed;
        // Quote markers, also nested ("> > text").
        while (!rest.empty() && rest.front() == '>') {
            rest = trimLeft(rest.substr(1));
            columns = 0;
        }
        // Heading markers: the line stays, plain.
        std::size_t hashes = 0;
        while (hashes < rest.size() && rest[hashes] == '#') {
            ++hashes;
        }
        if (hashes >= 1 && hashes <= 6 && (hashes == rest.size() || isBlank(rest[hashes]))) {
            rest = trimLeft(rest.substr(hashes));
        }

        AnswerLine shown;
        if (const std::optional<ListItem> item = listItem(rest, columns)) {
            shown.marker = item->marker;
            rest = item->rest;
        }
        shown.runs = InlineParser(rest, lineComplete).parse();
        if (shown.blank()) {
            addBlank(out);
            continue;
        }
        out.push_back(std::move(shown));
    }
    // addBlank() never starts with a blank line; this drops the one at the end.
    while (!out.empty() && out.back().blank()) {
        out.pop_back();
    }
    return answer;
}

AnswerText plainLines(std::string_view text) {
    AnswerText shown;
    for (const std::string_view line : splitLines(text)) {
        AnswerLine plain;
        if (!line.empty()) {
            plain.runs.push_back(Run{std::string(line), false});
        }
        shown.lines.push_back(std::move(plain));
    }
    return shown;
}

std::string plainText(const AnswerText& answer) {
    std::string out;
    for (std::size_t i = 0; i < answer.lines.size(); ++i) {
        if (i > 0) {
            out += '\n';
        }
        const AnswerLine& line = answer.lines[i];
        out += line.marker;
        for (const Run& run : line.runs) {
            out += run.text;
        }
    }
    return out;
}

}  // namespace curia::chatview
