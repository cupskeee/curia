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
