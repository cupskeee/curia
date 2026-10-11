// SPDX-License-Identifier: MIT
#include "bridge/line_splitter.h"

namespace curia::bridge {

std::vector<Line> LineSplitter::feed(std::string_view bytes) {
    std::vector<Line> lines;
    feed(bytes, [&lines](Line&& line) { lines.push_back(std::move(line)); });
    return lines;
}

void LineSplitter::reset() noexcept {
    tail_.clear();
    dropping_ = false;
}

}  // namespace curia::bridge
