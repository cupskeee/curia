// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <string_view>

#include "chatview/answer_text.h"

using curia::chatview::AnswerText;
using curia::chatview::kBulletMarker;
using curia::chatview::parseAnswer;
using curia::chatview::plainText;

namespace {

// One line per line; bold runs as <b>...</b>, a list marker as [marker].
std::string render(const AnswerText& answer) {
    std::string out;
    for (std::size_t i = 0; i < answer.lines.size(); ++i) {
        if (i > 0) {
            out += '\n';
        }
        const auto& line = answer.lines[i];
        if (!line.marker.empty()) {
            out += "[" + line.marker + "]";
        }
        for (const auto& run : line.runs) {
            out += run.bold ? "<b>" + run.text + "</b>" : run.text;
        }
    }
    return out;
}

std::string done(std::string_view text) {
    return render(parseAnswer(text, true));
}

std::string streaming(std::string_view text) {
    return render(parseAnswer(text, false));
}

const std::string kBullet(kBulletMarker);

}  // namespace

TEST_CASE("answer text: bold is rendered") {
    CHECK((done("Your **heir** is safe.") == "Your <b>heir</b> is safe."));
    CHECK((done("__Gold__ is low") == "<b>Gold</b> is low"));
    CHECK((done("**a** and **b**") == "<b>a</b> and <b>b</b>"));
    CHECK((done("**Pay**s off") == "<b>Pay</b>s off"));
    CHECK((done("**312 gold**, +7.8 a month") == "<b>312 gold</b>, +7.8 a month"));
    // Bold and emphasis together: bold stays, the emphasis markers go.
    CHECK((done("***Now***") == "<b>Now</b>"));
    CHECK((done("**see *this* now**") == "<b>see this now</b>"));
    // UTF-8 inside bold.
    CHECK((done("**\xC3\x86lfwynn Testd\xC3\xB3ttir**") ==
           "<b>\xC3\x86lfwynn Testd\xC3\xB3ttir</b>"));
}

TEST_CASE("answer text: single markers are stripped only when they wrap a word") {
    CHECK((done("an *important* point") == "an important point"));
    CHECK((done("an _important_ point") == "an important point"));
    CHECK((done("*Note:* pay first") == "Note: pay first"));
    // Inside words and names: kept.
    CHECK((done("snake_case_name stays") == "snake_case_name stays"));
    CHECK((done("a*b*c") == "a*b*c"));
    CHECK((done("Muh_ammad_ibn") == "Muh_ammad_ibn"));
    CHECK((done("\xC3\x86lf_wynn_") == "\xC3\x86lf_wynn_"));
    // A lone marker between blanks, or one that never closes.
    CHECK((done("5 * 3 = 15") == "5 * 3 = 15"));
    CHECK((done("a footnote*") == "a footnote*"));
    CHECK((done("*only one side") == "*only one side"));
    CHECK((done("2 ** 8") == "2 ** 8"));
    CHECK((done("a **** b") == "a **** b"));
    CHECK(done("****").empty());  // a rule
}

TEST_CASE("answer text: an unmatched ** of a complete answer is dropped") {
    CHECK((done("**Pay the army") == "Pay the army"));
    CHECK((done("Pay the army**") == "Pay the army"));
    CHECK((done("a **b") == "a b"));
}

TEST_CASE("answer text: headings, quotes, bullets, numbered lines") {
    CHECK((done("# Your realm") == "Your realm"));
    CHECK((done("### Gold and **debt**") == "Gold and <b>debt</b>"));
    CHECK((done("#hashtag stays") == "#hashtag stays"));
    CHECK((done("> a quote") == "a quote"));
    CHECK((done("> > nested") == "nested"));
    CHECK((done("- one\n* two") == "[" + kBullet + "]one\n[" + kBullet + "]two"));
    CHECK((done("-5 gold") == "-5 gold"));
    CHECK((done(kBullet + "already a bullet") == "[" + kBullet + "]already a bullet"));
    CHECK((done("  - nested item") == "[   " + kBullet + "]nested item"));
    // Numbered lines stay exactly as they are.
    CHECK((done("1. **Pay** the army: it is unhappy.") ==
           "[1. ]<b>Pay</b> the army: it is unhappy."));
    CHECK((done("2) Second") == "[2) ]Second"));
    CHECK((done("12. Twelfth") == "[12. ]Twelfth"));
    // Not a list: a year, no blank after the dot.
    CHECK((done("1066. The year") == "1066. The year"));
    CHECK((done("3.5 gold") == "3.5 gold"));
    CHECK((done("> - quoted bullet") == "[" + kBullet + "]quoted bullet"));
    CHECK((done("# 1. Heading number") == "[1. ]Heading number"));
}

TEST_CASE("answer text: code, links, rules, fences, tables, escapes") {
    CHECK((done("Open the `Vassals` window") == "Open the Vassals window"));
    CHECK((done("Use ``a `tick` here``") == "Use a `tick` here"));
    // The content of code is shown as it is, markers and all.
    CHECK((done("`**not bold**`") == "**not bold**"));
    CHECK((done("an unmatched ` tick") == "an unmatched  tick"));
    CHECK((done("See [the wiki](https://example.invalid/page) now") == "See the wiki now"));
    CHECK((done("![a map](x.png)") == "a map"));
    CHECK((done("[**bold link**](u)") == "<b>bold link</b>"));
    CHECK((done("[1] a reference") == "[1] a reference"));
    CHECK((done("a [bracket") == "a [bracket"));
    CHECK((done("before\n---\nafter") == "before\nafter"));
    CHECK((done("before\n\n***\n\nafter") == "before\n\nafter"));
    CHECK((done("a\n* * *\nb") == "a\nb"));
    CHECK((done("Title\n===") == "Title"));
    CHECK((done("```text\n**raw** _x_\n```\nafter") == "**raw** _x_\nafter"));
    CHECK((done("| a | **b** |\n|---|---|") == "| a | <b>b</b> |\n|---|---|"));
    CHECK((done("\\*not emphasis\\*") == "*not emphasis*"));
    CHECK((done("a \\ b") == "a \\ b"));
}

TEST_CASE("answer text: strikethrough loses its markers") {
    CHECK((done("Use ~~old~~ plans.") == "Use old plans."));
    CHECK((done("~~**bold** inside~~") == "<b>bold</b> inside"));
    CHECK((done("an ~~unmatched one") == "an unmatched one"));
    // A single tilde ("about") and longer runs are text.
    CHECK((done("about ~100 gold") == "about ~100 gold"));
    CHECK((done("a ~~~ b") == "a ~~~ b"));
    // While it arrives: no tilde shows up that the finished text drops.
    CHECK((streaming("Use ~") == "Use"));
    CHECK((streaming("Use ~~") == "Use"));
    CHECK((streaming("Use ~~ol") == "Use ol"));
    CHECK((streaming("Use ~~old~") == "Use old"));
    CHECK((streaming("Use ~~old~~ plans") == "Use old plans"));
    CHECK((streaming("about ~1") == "about ~1"));
}

TEST_CASE("answer text: blank lines") {
    CHECK((done("a\n\n\n\nb") == "a\n\nb"));
    CHECK((done("\n\na\n\n") == "a"));
    CHECK((done("a\r\nb") == "a\nb"));
    CHECK(parseAnswer("", true).lines.empty());
    CHECK(parseAnswer("\n\n", false).lines.empty());
    const AnswerText answer = parseAnswer("a\n\nb", true);
    REQUIRE(answer.lines.size() == 3);
    CHECK(answer.lines[1].blank());
}

TEST_CASE("answer text: while it arrives, partial markers are not shown and do not flicker") {
    // A marker at the very end may be the start of "**": not shown.
    CHECK((streaming("Your treasury holds *") == "Your treasury holds"));
    CHECK((streaming("Your treasury holds **") == "Your treasury holds"));
    // An opening ** is bold until it closes...
    CHECK((streaming("holds **312 go") == "holds <b>312 go</b>"));
    // ... and the half-arrived closing marker changes nothing.
    CHECK((streaming("holds **312 gold*") == "holds <b>312 gold</b>"));
    CHECK((streaming("holds **312 gold**") == "holds <b>312 gold</b>"));
    CHECK((streaming("holds **312 gold** and") == "holds <b>312 gold</b> and"));
    // A single marker is held back until its partner arrives (it would appear and vanish).
    CHECK((streaming("an *import") == "an import"));
    CHECK((streaming("an *important* one") == "an important one"));
    // A last line of markers only may still become a rule, a heading or a bullet.
    CHECK((streaming("text\n-") == "text"));
    CHECK((streaming("text\n--") == "text"));
    CHECK((streaming("text\n##") == "text"));
    CHECK((streaming("text\n>") == "text"));
    CHECK((streaming("text\n``") == "text"));
    CHECK((streaming("text\n- ") == "text"));
    CHECK((streaming("text\n- item") == "text\n[" + kBullet + "]item"));
    // Only the last line is unfinished: an earlier line with an unclosed ** is complete.
    CHECK((streaming("**not closed\nnext **op") == "not closed\nnext <b>op</b>"));
    // A link that is still arriving shows its text only.
    CHECK((streaming("see [the Vass") == "see the Vass"));
    CHECK((streaming("see [the Vassals window]") == "see the Vassals window"));
    CHECK((streaming("see [the Vassals window](https://exa") == "see the Vassals window"));
    CHECK((streaming("see [the Vassals window](https://example.invalid) now") ==
           "see the Vassals window now"));
    // An escape that is still arriving.
    CHECK((streaming("a \\") == "a"));
}

TEST_CASE("answer text: streaming never shows what the finished text drops") {
    // Every prefix of the canned smoke answer, cut at every byte, parses without a marker
    // character showing up that the finished text does not show (no flicker of * or #).
    const std::string full =
        "## Head\nYour **312 gold** and *care*.\n\n1. **Win over X**: send a `gift`.\n- a bullet";
    const std::string finished = done(full);
    for (std::size_t cut = 0; cut <= full.size(); ++cut) {
        const std::string shown = streaming(std::string_view(full).substr(0, cut));
        CAPTURE(cut);
        CAPTURE(shown);
        CHECK(shown.find('#') == std::string::npos);
        CHECK(shown.find('`') == std::string::npos);
        CHECK(shown.find('*') == std::string::npos);
    }
    CHECK((finished == "Head\nYour <b>312 gold</b> and care.\n\n[1. ]<b>Win over X</b>: send a "
                       "gift.\n[" +
                           kBullet + "]a bullet"));
}

TEST_CASE("answer text: UTF-8 is never split") {
    // Cyrillic and a combining-free Latin Extended name around markers.
    const std::string text = "**\xD0\xAF\xD1\x80\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB0\xD0\xB2** and "
                             "_Mu\xE1\xB8\xA5"
                             "ammad_";
    CHECK((done(text) == "<b>\xD0\xAF\xD1\x80\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB0\xD0\xB2</b> and "
                         "Mu\xE1\xB8\xA5"
                         "ammad"));
    // A marker next to a multi-byte letter counts as inside a word.
    CHECK((done("\xC3\xA9_x_\xC3\xA9") == "\xC3\xA9_x_\xC3\xA9"));
    // Malformed bytes pass through untouched.
    CHECK((done("bad \xFF\xFE bytes") == "bad \xFF\xFE bytes"));
}

TEST_CASE("answer text: the plain text for Copy") {
    const AnswerText answer =
        parseAnswer("## Plan\n1. **Pay** the army.\n- Watch `X`.\n\nDone", true);
    CHECK((plainText(answer) == "Plan\n1. Pay the army.\n" + kBullet + "Watch X.\n\nDone"));
    CHECK(plainText(parseAnswer("", true)).empty());
}
