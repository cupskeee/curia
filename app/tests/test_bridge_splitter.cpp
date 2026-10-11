// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "bridge/line_splitter.h"

using curia::bridge::Line;
using curia::bridge::LineSplitter;

namespace {

// The number of the spec (section 1), written out so that changing the constant fails a test.
constexpr std::size_t kLimit = 4096;
static_assert(LineSplitter::kMaxLineBytes == 4096);

std::vector<Line> splitAll(std::string_view bytes) {
    LineSplitter splitter;
    return splitter.feed(bytes);
}

std::vector<Line> splitInChunks(std::string_view bytes, std::size_t chunk) {
    LineSplitter splitter;
    std::vector<Line> out;
    for (std::size_t pos = 0; pos < bytes.size(); pos += chunk) {
        for (Line& line : splitter.feed(bytes.substr(pos, chunk))) {
            out.push_back(std::move(line));
        }
    }
    return out;
}

bool sameLines(const std::vector<Line>& a, const std::vector<Line>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].text != b[i].text || a[i].overlong != b[i].overlong) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("splitter: complete lines come out, the unterminated tail is held back") {
    LineSplitter splitter;
    auto lines = splitter.feed("one\ntwo\nthr");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "one");
    CHECK(lines[1].text == "two");
    CHECK_FALSE(lines[0].overlong);
    CHECK(splitter.pendingBytes() == 3);
    lines = splitter.feed("ee\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text == "three");
    CHECK(splitter.pendingBytes() == 0);
}

TEST_CASE("splitter: a tail without a newline is never returned") {
    LineSplitter splitter;
    CHECK(splitter.feed("CURIA1|7|END|2").empty());
    CHECK(splitter.pendingBytes() == 14);
}

TEST_CASE("splitter: empty lines are lines") {
    const auto lines = splitAll("\n\nx\n");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].text.empty());
    CHECK(lines[1].text.empty());
    CHECK(lines[2].text == "x");
}

TEST_CASE("splitter: exactly one CR before the LF is removed") {
    const auto lines = splitAll("a\r\nb\r\r\nc\rd\ne\r");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].text == "a");
    CHECK(lines[1].text == "b\r");   // only one of two
    CHECK(lines[2].text == "c\rd");  // a CR inside a line stays
    // "e\r" is an unterminated tail and is not returned.
}

TEST_CASE("splitter: a CR at the end of a feed waits for its LF") {
    LineSplitter splitter;
    CHECK(splitter.feed("x\r").empty());
    const auto lines = splitter.feed("\ny\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "x");
    CHECK(lines[1].text == "y");
}

TEST_CASE("splitter: a line of exactly 4096 bytes is not overlong") {
    const std::string body(kLimit, 'a');
    const auto lines = splitAll(body + "\n");
    REQUIRE(lines.size() == 1);
    CHECK_FALSE(lines[0].overlong);
    CHECK(lines[0].text == body);
}

TEST_CASE("splitter: a line of 4097 bytes is overlong and keeps its first 4096") {
    std::string body(kLimit, 'a');
    body += 'b';
    const auto lines = splitAll(body + "\nnext\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].overlong);
    CHECK(lines[0].text == std::string(kLimit, 'a'));
    CHECK_FALSE(lines[1].overlong);
    CHECK(lines[1].text == "next");
}

TEST_CASE("splitter: the limit is measured before the CR is removed") {
    // 4095 bytes + CR = 4096 physical bytes: not overlong, the CR goes.
    const std::string fits(kLimit - 1, 'a');
    auto lines = splitAll(fits + "\r\n");
    REQUIRE(lines.size() == 1);
    CHECK_FALSE(lines[0].overlong);
    CHECK(lines[0].text == fits);
    // 4096 bytes + CR = 4097 physical bytes: overlong; the head is the 4096 bytes, no CR removal
    // is applied to it (it is not directly before the LF any more).
    const std::string full(kLimit, 'a');
    lines = splitAll(full + "\r\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].overlong);
    CHECK(lines[0].text == full);
}

TEST_CASE("splitter: a CR as the last byte of an overlong head stays") {
    std::string body(kLimit - 1, 'a');
    body += '\r';
    body += "tail";
    const auto lines = splitAll(body + "\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].overlong);
    REQUIRE(lines[0].text.size() == kLimit);
    CHECK(lines[0].text.back() == '\r');
}

TEST_CASE("splitter: memory stays bounded however long a tail grows") {
    LineSplitter splitter;
    const std::string chunk(100000, 'x');
    for (int i = 0; i < 10; ++i) {
        CHECK(splitter.feed(chunk).empty());
        CHECK(splitter.pendingBytes() <= kLimit);
    }
    const auto lines = splitter.feed("\nok\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].overlong);
    CHECK(lines[0].text.size() == kLimit);
    CHECK(lines[1].text == "ok");
    CHECK_FALSE(lines[1].overlong);
}

TEST_CASE("splitter: reset forgets the tail and the dropping state") {
    LineSplitter splitter;
    splitter.feed("partial");
    splitter.reset();
    CHECK(splitter.pendingBytes() == 0);
    auto lines = splitter.feed("fresh\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text == "fresh");

    splitter.feed(std::string(kLimit + 10, 'z'));
    splitter.reset();
    lines = splitter.feed("after\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text == "after");
    CHECK_FALSE(lines[0].overlong);
}

TEST_CASE("splitter: chunk boundaries do not matter") {
    std::string bytes;
    bytes += "plain line\n";
    bytes += "with cr\r\n";
    bytes += "double cr\r\r\n";
    bytes += "\n";
    bytes += std::string(kLimit, 'a') + "\n";
    bytes += std::string(kLimit - 1, 'b') + "\r\n";
    bytes += std::string(kLimit + 1, 'c') + "\n";
    bytes += std::string(kLimit, 'd') + "\r\n";
    bytes += "CURIA1|7|END|2\n";
    bytes += "unterminated";

    const auto whole = splitAll(bytes);
    CHECK(whole.size() == 9);
    for (const std::size_t chunk : {1U, 2U, 3U, 7U, 64U, 4095U, 4096U, 4097U}) {
        CAPTURE(chunk);
        CHECK(sameLines(whole, splitInChunks(bytes, chunk)));
    }
}

TEST_CASE("splitter: every two-part split of a stream gives the same lines") {
    std::string bytes = "ab\r\ncd\n\r\n";
    bytes += std::string(kLimit - 2, 'x') + "\r\n";
    bytes += std::string(kLimit + 2, 'y') + "\r\n";
    bytes += "end\n";
    const auto whole = splitAll(bytes);
    for (std::size_t cut = 0; cut <= bytes.size(); cut += 1) {
        LineSplitter splitter;
        std::vector<Line> lines = splitter.feed(std::string_view(bytes).substr(0, cut));
        for (Line& line : splitter.feed(std::string_view(bytes).substr(cut))) {
            lines.push_back(std::move(line));
        }
        REQUIRE(sameLines(whole, lines));
    }
}

TEST_CASE("splitter: the callback form hands out the same lines as the vector form") {
    std::string bytes = "a\r\n\nCURIA1|7|END|2\n";
    bytes += std::string(kLimit + 5, 'q') + "\n";
    bytes += "tail";
    const auto expected = splitAll(bytes);
    LineSplitter splitter;
    std::vector<Line> got;
    splitter.feed(bytes, [&got](Line&& line) { got.push_back(std::move(line)); });
    CHECK(sameLines(expected, got));
    CHECK(splitter.pendingBytes() == 4);
}

TEST_CASE("splitter: the callback form keeps no per-line storage for a large block of newlines") {
    // 2 MiB of empty lines: nothing is accumulated, every line reaches the callback.
    LineSplitter splitter;
    const std::string block(2U * 1024U * 1024U, '\n');
    std::size_t lines = 0;
    splitter.feed(block, [&lines](Line&& line) {
        if (line.text.empty() && !line.overlong) {
            ++lines;
        }
    });
    CHECK(lines == block.size());
    CHECK(splitter.pendingBytes() == 0);
}
