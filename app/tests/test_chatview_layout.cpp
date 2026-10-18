// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "chatview/answer_text.h"
#include "chatview/text_layout.h"
#include "chatview/utf8.h"

using curia::chatview::AnswerLine;
using curia::chatview::AnswerText;
using curia::chatview::kBulletMarker;
using curia::chatview::layoutText;
using curia::chatview::MeasureFn;
using curia::chatview::parseAnswer;
using curia::chatview::plainLines;
using curia::chatview::Run;
using curia::chatview::TextLayout;

namespace {

// Every character (code point) is 1 wide, a bold one 2 wide: positions are easy to predict.
float fixedWidth(std::string_view text, bool bold) {
    return static_cast<float>(curia::chatview::decodeUtf8(text).size()) * (bold ? 2.0F : 1.0F);
}

const MeasureFn kMeasure = fixedWidth;

// The lines as text, bold segments as <b>...</b>.
std::vector<std::string> lines(const TextLayout& layout) {
    std::vector<std::string> out;
    for (const auto& line : layout.lines) {
        std::string text;
        for (const auto& segment : line.segments) {
            text += segment.bold ? "<b>" + segment.text + "</b>" : segment.text;
        }
        out.push_back(text);
    }
    return out;
}

AnswerText one(std::vector<Run> runs, std::string marker = {}) {
    AnswerText text;
    AnswerLine line;
    line.marker = std::move(marker);
    line.runs = std::move(runs);
    text.lines.push_back(std::move(line));
    return text;
}

}  // namespace

TEST_CASE("layout: words wrap at blanks, the blank at a break is not drawn") {
    const TextLayout layout = layoutText(one({{"the quick brown fox jumps", false}}), 10, kMeasure);
    CHECK((lines(layout) == std::vector<std::string>{"the quick", "brown fox", "jumps"}));
    CHECK(layout.width == doctest::Approx(9));
    // A word that exactly fills the line stays on it.
    CHECK((lines(layoutText(one({{"abcde fghij", false}}), 11, kMeasure)) ==
           std::vector<std::string>{"abcde fghij"}));
    CHECK((lines(layoutText(one({{"abcde fghij", false}}), 10, kMeasure)) ==
           std::vector<std::string>{"abcde", "fghij"}));
    // Several blanks between words count; blanks at the end of a line are not drawn.
    CHECK((lines(layoutText(one({{"ab   cd  ", false}}), 20, kMeasure)) ==
           std::vector<std::string>{"ab   cd"}));
}

TEST_CASE("layout: explicit lines and blank lines are kept") {
    const TextLayout layout = layoutText(plainLines("one\n\ntwo\n"), 20, kMeasure);
    CHECK((lines(layout) == std::vector<std::string>{"one", "", "two", ""}));
    // An indent the player typed is kept at the start of the line.
    CHECK((lines(layoutText(plainLines("  indented"), 20, kMeasure)) ==
           std::vector<std::string>{"  indented"}));
}

TEST_CASE("layout: a word longer than the line breaks between characters") {
    CHECK((lines(layoutText(one({{"abcdefghijkl", false}}), 5, kMeasure)) ==
           std::vector<std::string>{"abcde", "fghij", "kl"}));
    // After a short word, the long word starts on its own line first.
    CHECK((lines(layoutText(one({{"ab cdefghij", false}}), 5, kMeasure)) ==
           std::vector<std::string>{"ab", "cdefg", "hij"}));
    // Code points, never bytes: two-byte and three-byte characters stay whole.
    const std::string word = "\xC3\xBE\xC3\xB0\xC3\xB6\xD0\xAF\xE1\xB8\xA5\xC3\xBC";  // þðöЯḥü
    const TextLayout layout = layoutText(one({{word, false}}), 4, kMeasure);
    REQUIRE(layout.lines.size() == 2);
    CHECK((lines(layout) ==
           std::vector<std::string>{"\xC3\xBE\xC3\xB0\xC3\xB6\xD0\xAF", "\xE1\xB8\xA5\xC3\xBC"}));
    // Width 0 or less: one character per line, and the layout still ends.
    CHECK((lines(layoutText(one({{"abc", false}}), 0, kMeasure)) ==
           std::vector<std::string>{"a", "b", "c"}));
    CHECK((lines(layoutText(one({{"ab", false}}), -3, kMeasure)) ==
           std::vector<std::string>{"a", "b"}));
}

TEST_CASE("layout: bold and plain mix inside a line and inside a word") {
    const TextLayout layout =
        layoutText(parseAnswer("Your **heir** is **safe**s now", true), 100, kMeasure);
    REQUIRE(layout.lines.size() == 1);
    CHECK((lines(layout) == std::vector<std::string>{"Your <b>heir</b> is <b>safe</b>s now"}));
    // Positions add up: "Your " is 5 wide, "heir" 8 (bold), " is " 4, "safe" 8, "s now" 5.
    const auto& segments = layout.lines[0].segments;
    REQUIRE(segments.size() == 5);
    CHECK(segments[1].x == doctest::Approx(5));
    CHECK(segments[1].width == doctest::Approx(8));
    CHECK(segments[2].x == doctest::Approx(13));
    CHECK(segments[3].x == doctest::Approx(17));
    CHECK(segments[4].x == doctest::Approx(25));
    CHECK(layout.width == doctest::Approx(30));
    // "**safe**s" is one word: it moves to the next line as a whole.
    CHECK((lines(layoutText(parseAnswer("abcdefghijkl **safe**s", true), 14, kMeasure)) ==
           std::vector<std::string>{"abcdefghijkl", "<b>safe</b>s"}));
    // A long bold word breaks too (each bold character is 2 wide).
    CHECK((lines(layoutText(parseAnswer("**abcdef**", true), 6, kMeasure)) ==
           std::vector<std::string>{"<b>abc</b>", "<b>def</b>"}));
}

TEST_CASE("layout: list lines get a hanging indent") {
    const TextLayout layout =
        layoutText(parseAnswer("1. Pay the army before it leaves", true), 14, kMeasure);
    CHECK((lines(layout) == std::vector<std::string>{"1. Pay the", "army before", "it leaves"}));
    REQUIRE(layout.lines.size() == 3);
    CHECK(layout.lines[1].segments[0].x == doctest::Approx(3));
    CHECK(layout.lines[2].segments[0].x == doctest::Approx(3));
    // A bullet.
    const TextLayout bullet = layoutText(parseAnswer("- one two three", true), 9, kMeasure);
    CHECK((lines(bullet) ==
           std::vector<std::string>{std::string(kBulletMarker) + "one two", "three"}));
    REQUIRE(bullet.lines.size() == 2);
    CHECK(bullet.lines[1].segments[0].x == doctest::Approx(2));
    // A marker wider than half the line: no hanging indent.
    const TextLayout wide = layoutText(one({{"aa bb", false}}, "1234. "), 8, kMeasure);
    REQUIRE(wide.lines.size() == 2);
    CHECK(wide.lines[1].segments[0].x == doctest::Approx(0));
    // A long word right after the marker breaks after it, not on a line of its own.
    CHECK((lines(layoutText(one({{"abcdefgh", false}}, "1. "), 8, kMeasure)) ==
           std::vector<std::string>{"1. abcde", "fgh"}));
}

TEST_CASE("layout: deterministic") {
    const AnswerText text = parseAnswer(
        "## Head\nYour **312 gold** grows.\n\n1. **Win over X**: send a gift.\n- a bullet", true);
    const TextLayout a = layoutText(text, 17, kMeasure);
    const TextLayout b = layoutText(text, 17, kMeasure);
    CHECK((lines(a) == lines(b)));
    REQUIRE(a.lines.size() == b.lines.size());
    for (std::size_t i = 0; i < a.lines.size(); ++i) {
        REQUIRE(a.lines[i].segments.size() == b.lines[i].segments.size());
        for (std::size_t k = 0; k < a.lines[i].segments.size(); ++k) {
            CHECK(a.lines[i].segments[k].x == b.lines[i].segments[k].x);
        }
    }
    // No line is wider than the limit (all words here fit on a line).
    for (const auto& line : a.lines) {
        if (!line.segments.empty()) {
            const auto& last = line.segments.back();
            CHECK(last.x + last.width <= 17.0F);
        }
    }
}
