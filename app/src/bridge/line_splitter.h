// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
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
    // memory held is bounded by kMaxLineBytes however long the tail grows. The result itself holds
    // every line of `bytes` at once (32 bytes per empty line); a caller that feeds a large block
    // should use the callback form below.
    std::vector<Line> feed(std::string_view bytes);

    // The same, handing each completed line to `onLine(Line&&)` as it completes, so that the memory
    // used is bounded by the longest line however large `bytes` is.
    template <typename OnLine> void feed(std::string_view bytes, OnLine&& onLine);

    // The log was truncated: forget the unterminated tail.
    void reset() noexcept;

    std::size_t pendingBytes() const noexcept { return tail_.size(); }

private:
    std::string tail_;
    // The current line already exceeded kMaxLineBytes: keep the head, drop bytes up to the next
    // '\n'.
    bool dropping_ = false;
};

template <typename OnLine> void LineSplitter::feed(std::string_view bytes, OnLine&& onLine) {
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const std::size_t newline = bytes.find('\n', pos);
        const std::size_t end = newline == std::string_view::npos ? bytes.size() : newline;
        if (!dropping_) {
            const std::size_t room = kMaxLineBytes - tail_.size();
            const std::size_t take = std::min(room, end - pos);
            tail_.append(bytes.substr(pos, take));
            // A byte beyond the limit makes the line overlong even if it is a '\r' that would have
            // been removed: the limit is measured before the '\r' removal.
            if (take < end - pos) {
                dropping_ = true;
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        Line line;
        line.overlong = dropping_;
        if (!dropping_ && !tail_.empty() && tail_.back() == '\r') {
            tail_.pop_back();
        }
        line.text = std::move(tail_);
        tail_.clear();
        dropping_ = false;
        pos = newline + 1;
        onLine(std::move(line));
    }
}

}  // namespace curia::bridge
