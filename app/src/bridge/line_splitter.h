// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace curia::bridge {

// One complete physical line of the log (docs/wire-format.md section 1).
struct Line {
    // Without the '\n' and, for a normal line, without the one '\r' directly before it. For an
    // overlong line this is the head: its first kMaxLineBytes bytes, with no '\r' removal.
    std::string text;
    // The physical line (before '\r' removal, without '\n') was longer than kMaxLineBytes; the rest
    // of it was dropped.
    bool overlong = false;
};

// Turns appended log bytes into complete physical lines. Pure: no I/O. Which bytes arrive in which
// feed() call does not change the lines produced.
class LineSplitter {
public:
    static constexpr std::size_t kMaxLineBytes = 4096;

    // Returns the lines completed by `bytes`. An unterminated tail is held back (never returned);
    // memory held is bounded by kMaxLineBytes however long the tail grows.
    std::vector<Line> feed(std::string_view bytes);

    // The log was truncated: forget the unterminated tail.
    void reset() noexcept;

    std::size_t pendingBytes() const noexcept { return tail_.size(); }

private:
    std::string tail_;
    // The current line already exceeded kMaxLineBytes: keep the head, drop bytes up to the next
    // '\n'.
    bool dropping_ = false;
};

}  // namespace curia::bridge
