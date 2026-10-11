// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

#include "providers/provider_util.h"

using curia::providers::kMaxJsonDepth;
using curia::providers::parseBoundedJson;
using curia::providers::printableExcerpt;

namespace {

std::string nestedArrays(int depth) {
    return std::string(static_cast<std::size_t>(depth), '[') +
           std::string(static_cast<std::size_t>(depth), ']');
}

}  // namespace

TEST_CASE("parseBoundedJson: ordinary values") {
    const auto parsed = parseBoundedJson(R"({"a":[1,2,{"b":"c"}],"d":null})");
    REQUIRE(parsed.has_value());
    const nlohmann::json object = parsed.value_or(nlohmann::json());
    CHECK(object.is_object());
    CHECK(object["a"].size() == 3);
    CHECK(parseBoundedJson("[]").has_value());
    CHECK(parseBoundedJson("\"text\"").has_value());
    CHECK(parseBoundedJson("12").has_value());
}

TEST_CASE("parseBoundedJson: broken input is nullopt, never a throw") {
    CHECK_FALSE(parseBoundedJson("").has_value());
    CHECK_FALSE(parseBoundedJson("{not json").has_value());
    CHECK_FALSE(parseBoundedJson("{\"a\":1} trailing").has_value());
    CHECK_FALSE(parseBoundedJson("{\"a\":").has_value());
    CHECK_FALSE(parseBoundedJson(std::string("\0", 1)).has_value());
}

TEST_CASE("parseBoundedJson: invalid UTF-8 in a string is refused") {
    CHECK_FALSE(parseBoundedJson("{\"a\":\"\xFF\"}").has_value());
    CHECK_FALSE(parseBoundedJson("{\"a\":\"\xC3\"}").has_value());
    CHECK(parseBoundedJson("{\"a\":\"\xC3\xA9\"}").has_value());
}

TEST_CASE("parseBoundedJson: the depth limit") {
    // The limit is a security constant: pin its value, not only its use.
    static_assert(kMaxJsonDepth == 32);
    CHECK(parseBoundedJson(nestedArrays(32)).has_value());
    CHECK_FALSE(parseBoundedJson(nestedArrays(33)).has_value());
    CHECK(parseBoundedJson(nestedArrays(kMaxJsonDepth)).has_value());
    CHECK_FALSE(parseBoundedJson(nestedArrays(kMaxJsonDepth + 1)).has_value());
    // Never reaches the recursion of anything: 10000 openers and nothing else.
    CHECK_FALSE(parseBoundedJson(std::string(10000, '[')).has_value());
    CHECK_FALSE(parseBoundedJson(std::string(10000, '{')).has_value());
    // A custom limit.
    CHECK(parseBoundedJson("[[1]]", 100, 2).has_value());
    CHECK_FALSE(parseBoundedJson("[[[1]]]", 100, 2).has_value());
}

TEST_CASE("parseBoundedJson: brackets inside strings do not count toward the depth") {
    const std::string lots(200, '[');
    CHECK(parseBoundedJson("[\"" + lots + "\"]").has_value());
    // An escaped quote does not end the string.
    CHECK(parseBoundedJson("[\"\\\"" + lots + "\"]").has_value());
    // An escaped backslash does not escape the closing quote.
    CHECK(parseBoundedJson("[\"\\\\\",1]").has_value());
    CHECK_FALSE(parseBoundedJson("[\"\\\\\"" + lots + "]").has_value());
}

TEST_CASE("parseBoundedJson: the size limit") {
    const std::string text = "[1,2,3]";
    CHECK(parseBoundedJson(text, text.size()).has_value());
    CHECK_FALSE(parseBoundedJson(text, text.size() - 1).has_value());
}

TEST_CASE("printableExcerpt: plain text passes through") {
    CHECK((printableExcerpt("hello world", 100) == "hello world"));
    CHECK((printableExcerpt("caf\xC3\xA9 \xE2\x82\xAC"
                            "5 \xF0\x9F\x98\x80",
                            100) == "caf\xC3\xA9 \xE2\x82\xAC"
                                    "5 \xF0\x9F\x98\x80"));
    CHECK(printableExcerpt("", 10).empty());
    CHECK(printableExcerpt("abc", 0).empty());
}

TEST_CASE("printableExcerpt: cut at a character boundary") {
    // "a" + e-acute (2 bytes) + euro (3) + grinning face (4)
    const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    CHECK((printableExcerpt(text, 1) == "a"));
    CHECK((printableExcerpt(text, 2) == "a"));  // the 2-byte character does not fit after "a"
    CHECK((printableExcerpt(text, 3) == "a\xC3\xA9"));
    CHECK((printableExcerpt(text, 5) == "a\xC3\xA9"));
    CHECK((printableExcerpt(text, 6) == "a\xC3\xA9\xE2\x82\xAC"));
    CHECK((printableExcerpt(text, 9) == "a\xC3\xA9\xE2\x82\xAC"));
    CHECK((printableExcerpt(text, 10) == text));
    CHECK((printableExcerpt(text, 100) == text));
}

TEST_CASE("printableExcerpt: control characters become spaces, ends are trimmed") {
    CHECK((printableExcerpt("a\nb\r\nc\td\x01"
                            "e\x7F"
                            "f",
                            100) == "a b  c d e f"));
    CHECK((printableExcerpt("  \n lead and trail \r\n ", 100) == "lead and trail"));
    CHECK(printableExcerpt(" \n\t ", 100).empty());
    // C1 controls (U+0085 is NEL) too.
    CHECK((printableExcerpt("a\xC2\x85"
                            "b",
                            100) == "a b"));
    // A no-break space (U+00A0) is not a control.
    CHECK((printableExcerpt("a\xC2\xA0"
                            "b",
                            100) == "a\xC2\xA0"
                                    "b"));
}

TEST_CASE("printableExcerpt: bytes that are not UTF-8 become question marks") {
    CHECK((printableExcerpt("a\xFF"
                            "b",
                            100) == "a?b"));
    CHECK((printableExcerpt("a\x80"
                            "b",
                            100) == "a?b"));                       // a stray continuation byte
    CHECK((printableExcerpt("\xC0\xAF", 100) == "??"));            // overlong
    CHECK((printableExcerpt("\xED\xA0\x80", 100) == "???"));       // a surrogate
    CHECK((printableExcerpt("\xF4\x90\x80\x80", 100) == "????"));  // above U+10FFFF
    CHECK((printableExcerpt("ab\xE2\x82", 100) == "ab??"));        // cut off at the end
    CHECK((printableExcerpt("\xF8\x88\x80\x80\x80", 100) == "?????"));
}

TEST_CASE("printableExcerpt: a lead byte followed by something that is not a continuation") {
    // The following byte is kept as it is (it may itself be fine).
    CHECK((printableExcerpt("\xC3"
                            "A",
                            100) == "?A"));
    CHECK((printableExcerpt("\xE2\x82"
                            "A",
                            100) == "??A"));
    CHECK((printableExcerpt("\xE2"
                            "AB",
                            100) == "?AB"));
    CHECK((printableExcerpt("\xF0\x9F\x98"
                            "A",
                            100) == "???A"));
    CHECK((printableExcerpt("\xC3\n", 100) == "?"));  // the newline becomes a space, then trimmed
    CHECK((printableExcerpt("\xC3\xC3\xA9", 100) == "?\xC3\xA9"));
}

TEST_CASE("printableExcerpt: overlong forms and the edges of the ranges") {
    CHECK((printableExcerpt("\xE0\x80\x80", 100) == "???"));           // overlong, 3 bytes
    CHECK((printableExcerpt("\xE0\x9F\xBF", 100) == "???"));           // overlong, up to U+07FF
    CHECK((printableExcerpt("\xE0\xA0\x80", 100) == "\xE0\xA0\x80"));  // U+0800, the lowest
    CHECK((printableExcerpt("\xF0\x80\x80\x80", 100) == "????"));      // overlong, 4 bytes
    CHECK((printableExcerpt("\xF0\x8F\xBF\xBF", 100) == "????"));      // overlong, up to U+FFFF
    CHECK((printableExcerpt("\xF0\x90\x80\x80", 100) == "\xF0\x90\x80\x80"));  // U+10000
    CHECK((printableExcerpt("\xED\x9F\xBF", 100) == "\xED\x9F\xBF"));  // U+D7FF, below surrogates
    CHECK((printableExcerpt("\xED\xA0\x80", 100) == "???"));           // U+D800, a surrogate
    CHECK((printableExcerpt("\xEE\x80\x80", 100) == "\xEE\x80\x80"));  // U+E000, above them
    CHECK((printableExcerpt("\xF4\x8F\xBF\xBF", 100) == "\xF4\x8F\xBF\xBF"));  // U+10FFFF
    CHECK((printableExcerpt("\xC1\xBF", 100) == "??"));  // overlong 2-byte (0xC0 and 0xC1)
    CHECK((printableExcerpt("\xC2\x80", 100).empty()));  // the lowest 2-byte value is a C1 control
}

TEST_CASE("printableExcerpt: pseudo-random bytes always give valid UTF-8 within the bound") {
    // A fixed-seed generator, so a failure repeats. Bytes drawn from the interesting ranges so
    // that lead bytes are often followed by ASCII, continuations and other lead bytes.
    std::uint32_t state = 12345;
    const auto next = [&state]() {
        state = state * 1664525U + 1013904223U;
        return state >> 16;
    };
    constexpr std::array<unsigned char, 18> kPool = {0x41, 0x0A, 0x7F, 0x80, 0x9F, 0xA0,
                                                     0xBF, 0xC0, 0xC2, 0xDF, 0xE0, 0xE2,
                                                     0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xFF};
    for (int round = 0; round < 500; ++round) {
        std::string text;
        const std::uint32_t length = next() % 24;
        for (std::uint32_t i = 0; i < length; ++i) {
            text.push_back(static_cast<char>(kPool[next() % kPool.size()]));
        }
        const std::size_t bound = next() % 30;
        const std::string out = printableExcerpt(text, bound);
        CAPTURE(round);
        CHECK(out.size() <= bound);
        // As a JSON string it parses only if it is valid UTF-8 with no raw control characters.
        CHECK(parseBoundedJson("\"" + out + "\"").has_value());
    }
}

TEST_CASE("printableExcerpt: the result is always within the bound and valid UTF-8") {
    std::string text;
    for (int i = 0; i < 600; ++i) {
        const auto c = static_cast<char>(i);  // wraps: every byte value, twice and more
        if (c != '"' && c != '\\') {
            text.push_back(c);
        }
    }
    for (std::size_t bound = 0; bound < 80; ++bound) {
        const std::string out = printableExcerpt(text, bound);
        CHECK(out.size() <= bound);
        // As a JSON string it parses only if it is valid UTF-8 with no raw control characters.
        CHECK(parseBoundedJson("\"" + out + "\"").has_value());
    }
}
