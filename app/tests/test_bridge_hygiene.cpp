// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>

#include "bridge/text_hygiene.h"

using curia::bridge::cleanText;
using curia::bridge::repairUtf8;

namespace {

// ^U of the spec's worked sample.
const std::string kU(1, '\x15');
const std::string kReplacement = "\xEF\xBF\xBD";

}  // namespace

TEST_CASE("hygiene: the worked sample of the spec") {
    const std::string raw = "Install " + kU + "ONCLICK:CHARACTER,1 " + kU + "TOOLTIP:CHARACTER,1 " +
                            kU + "L Test Ruler" + kU + "!" + kU + "!" + kU + "! on the " + kU +
                            "ONCLICK:TITLE,2 " + kU + "TOOLTIP:LANDED_TITLE,2 " + kU +
                            "L; Test Realm" + kU + "!" + kU + "!" + kU + "! Throne";
    CHECK(cleanText(raw) == "Install Test Ruler on the Test Realm Throne");
}

TEST_CASE("hygiene: clean text passes through unchanged, non-ASCII included") {
    CHECK(cleanText("Test Ruler One") == "Test Ruler One");
    CHECK(cleanText("") == "");
    const std::string accented = "Test \xC3\xA9\xC3\xB0 \xE2\x82\xAC \xF0\x9F\x91\x91";
    CHECK(cleanText(accented) == accented);
}

TEST_CASE("hygiene step 1: invalid UTF-8 becomes U+FFFD") {
    CHECK(repairUtf8("a\x80z") == "a" + kReplacement + "z");
    CHECK(repairUtf8("a\xFFz") == "a" + kReplacement + "z");
    // An overlong form, a surrogate and a code point above U+10FFFF are invalid.
    CHECK(repairUtf8("\xC0\xAF") == kReplacement + kReplacement);
    CHECK(repairUtf8("\xED\xA0\x80") == kReplacement + kReplacement + kReplacement);
    CHECK(repairUtf8("\xF4\x90\x80\x80") ==
          kReplacement + kReplacement + kReplacement + kReplacement);
    // A truncated sequence is one maximal invalid subpart; the next byte is kept.
    CHECK(repairUtf8("\xE2\x82z") == kReplacement + "z");
    CHECK(repairUtf8("\xE2\x82") == kReplacement);
    CHECK(repairUtf8("\xF0\x9F\x91") == kReplacement);
    // Valid text is untouched.
    CHECK(repairUtf8("\xC3\xA9") == "\xC3\xA9");
    // The replacement itself survives the cleaning.
    CHECK(cleanText("a\x80z") == "a" + kReplacement + "z");
}

TEST_CASE("hygiene step 2: the byte pair 0x15 0x21 is deleted") {
    CHECK(cleanText("a" + kU + "!b") == "ab");
    CHECK(cleanText("a" + kU + "!" + kU + "!b") == "ab");
    // One pass, no re-scan: 0x15 0x15 0x21 0x21 leaves 0x15 0x21 after the first pass, and step 4
    // removes the stray 0x15 but keeps the '!'.
    CHECK(cleanText(kU + kU + "!!") == "!");
    // A '!' without 0x15 stays.
    CHECK(cleanText("Hello!") == "Hello!");
}

TEST_CASE("hygiene step 3: known tag words are removed with one following space") {
    CHECK(cleanText(kU + "L Name") == "Name");
    CHECK(cleanText(kU + "L; Name") == "Name");
    CHECK(cleanText("A " + kU + "ONCLICK:CHARACTER,5 B") == "A B");
    CHECK(cleanText("A " + kU + "TOOLTIP:X,1 B") == "A B");
    // Only one space is removed with the tag; the collapse of step 5 does the rest anyway.
    CHECK(cleanText("A" + kU + "L  B") == "A B");
}

TEST_CASE("hygiene step 3: a tag word at the end of the text") {
    CHECK(cleanText("Name " + kU + "L") == "Name");
    CHECK(cleanText("Name " + kU + "L;") == "Name");
    CHECK(cleanText("Name " + kU + "ONCLICK:CHARACTER,1") == "Name");
    CHECK(cleanText(kU + "TOOLTIP:") == "");
}

TEST_CASE("hygiene step 3: unknown tag words are not removed, only the 0x15 goes") {
    CHECK(cleanText(kU + "Lx Name") == "Lx Name");
    CHECK(cleanText(kU + "l Name") == "l Name");
    CHECK(cleanText(kU + "LL Name") == "LL Name");
    CHECK(cleanText(kU + "onclick:X Name") == "onclick:X Name");
    CHECK(cleanText(kU + " Name") == "Name");
    CHECK(cleanText("Name" + kU) == "Name");
    // A tag word is the maximal run of non-space bytes, so "L" directly followed by another 0x15
    // and more text is not the word "L".
    CHECK(cleanText(kU + "L" + kU + "x y") == "Lx y");
}

TEST_CASE("hygiene step 4: control, bidi and format code points are deleted") {
    CHECK(cleanText(std::string("a\x01") + "b") == "ab");
    CHECK(cleanText("a\tb") == "ab");
    CHECK(cleanText("a\nb") == "ab");
    CHECK(cleanText("a\rb") == "ab");
    CHECK(cleanText(std::string("a\x7F") + "b") == "ab");
    CHECK(cleanText("a\xC2\x80z") == "az");  // U+0080
    CHECK(cleanText("a\xC2\x9F"
                    "z") == "az");                           // U+009F
    CHECK(cleanText("a\xC2\xA0z") == "a\xC2\xA0z");          // U+00A0 stays
    CHECK(cleanText("a\xE2\x80\xA8z") == "az");              // U+2028
    CHECK(cleanText("a\xE2\x80\xA9z") == "az");              // U+2029
    CHECK(cleanText("a\xE2\x80\xAAz") == "az");              // U+202A
    CHECK(cleanText("a\xE2\x80\xAEz") == "az");              // U+202E
    CHECK(cleanText("a\xE2\x80\xAFz") == "a\xE2\x80\xAFz");  // U+202F stays
    CHECK(cleanText("a\xE2\x81\xA6z") == "az");              // U+2066
    CHECK(cleanText("a\xE2\x81\xA9z") == "az");              // U+2069
    CHECK(cleanText("a\xE2\x81\xAAz") == "a\xE2\x81\xAAz");  // U+206A stays
}

TEST_CASE("hygiene step 5: runs of spaces collapse and the ends are trimmed") {
    CHECK(cleanText("  a   b  ") == "a b");
    CHECK(cleanText("   ") == "");
    CHECK(cleanText("a \x01 b") == "a b");  // a removed code point between spaces
    CHECK(cleanText(std::string("\x01") + " a") == "a");
}

TEST_CASE("hygiene: the markup of a pipe-bearing name is just text") {
    CHECK(cleanText("Test | Name") == "Test | Name");
}

// ---- pins for the edges of each step (review findings) -----------------------------------------

TEST_CASE("hygiene step 3: the tag word eats exactly one following U+0020") {
    // With two spaces after the tag the collapse of step 5 hides the difference, so a single space
    // between the neighbours is what shows that the tag took its space.
    CHECK(cleanText("A" + kU + "L B") == "AB");
    CHECK(cleanText("A" + kU + "L; B") == "AB");
    CHECK(cleanText("A" + kU + "ONCLICK:X,1 B") == "AB");
    CHECK(cleanText("A " + kU + "L B") == "A B");
}

TEST_CASE("hygiene step 3: only U+0020 ends a tag word, a tab does not") {
    // The tab is part of the "word" L<TAB>x, which is not a known tag; the 0x15 and the tab go in
    // step 4.
    CHECK(cleanText(kU + "L\tx") == "Lx");
}

TEST_CASE("hygiene step 4: U+001F goes, and the neighbours of the deleted ranges stay") {
    CHECK(cleanText(std::string("a\x1F") + "b") == "ab");
    CHECK(cleanText(std::string("a\x1E") + "b") == "ab");
    CHECK(cleanText("a\x20"
                    "b") == "a b");  // U+0020 is the separator, not a control character
    // Zero-width and format characters outside the listed ranges are kept.
    CHECK(cleanText("a\xE2\x80\x8B"
                    "z") == "a\xE2\x80\x8B"
                            "z");  // U+200B
    CHECK(cleanText("a\xE2\x80\x8E"
                    "z") == "a\xE2\x80\x8E"
                            "z");  // U+200E
    CHECK(cleanText("a\xE2\x80\xA7"
                    "z") == "a\xE2\x80\xA7"
                            "z");  // U+2027
    CHECK(cleanText("a\xE2\x81\xA0"
                    "z") == "a\xE2\x81\xA0"
                            "z");  // U+2060
    CHECK(cleanText("a\xC2\xAD"
                    "z") == "a\xC2\xAD"
                            "z");  // U+00AD
}

TEST_CASE("hygiene step 1: overlong forms and out-of-range lead bytes give one U+FFFD per byte") {
    CHECK(repairUtf8("\xE0\x80\x80") == kReplacement + kReplacement + kReplacement);
    CHECK(repairUtf8("\xF0\x80\x80\x80") ==
          kReplacement + kReplacement + kReplacement + kReplacement);
    CHECK(repairUtf8("\xF5\x80\x80\x80") ==
          kReplacement + kReplacement + kReplacement + kReplacement);
    CHECK(repairUtf8("\xF7\xBF\xBF\xBF") ==
          kReplacement + kReplacement + kReplacement + kReplacement);
    CHECK(repairUtf8("\xC1\xBF") == kReplacement + kReplacement);
    // The lowest and highest valid forms of each length stay.
    CHECK(repairUtf8("\xC2\x80") == "\xC2\x80");
    CHECK(repairUtf8("\xE0\xA0\x80") == "\xE0\xA0\x80");
    CHECK(repairUtf8("\xF0\x90\x80\x80") == "\xF0\x90\x80\x80");
    CHECK(repairUtf8("\xF4\x8F\xBF\xBF") == "\xF4\x8F\xBF\xBF");
}

TEST_CASE("hygiene step 3: a long run of 0x15 is handled in linear time") {
    // Scanning to the end of the tag word for every 0x15 made this input quadratic: 200,000 bytes
    // took tens of seconds; the shared run end makes it a single pass.
    const std::string run(200000, '\x15');
    CHECK(cleanText(run) == "");
    // Only the last 0x15 has a known tag word ("L"); the others are followed by longer words.
    CHECK(cleanText(run + "L x") == "x");
    // Every word here ends at the one space and is unknown, so only the 0x15 bytes go.
    std::string stairs;
    std::string expected;
    for (int i = 0; i < 20000; ++i) {
        stairs += kU + "Lx";
        expected += "Lx";
    }
    CHECK(cleanText(stairs + " y") == expected + " y");
}
