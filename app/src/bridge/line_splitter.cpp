// SPDX-License-Identifier: MIT
#include "bridge/line_splitter.h"

#include <algorithm>

namespace curia::bridge {

std::vector<Line> LineSplitter::feed(std::string_view bytes) {
    std::vector<Line> lines;
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
        lines.push_back(std::move(line));
        tail_.clear();
        dropping_ = false;
        pos = newline + 1;
    }
    return lines;
}

void LineSplitter::reset() noexcept {
    tail_.clear();
    dropping_ = false;
}

}  // namespace curia::bridge
