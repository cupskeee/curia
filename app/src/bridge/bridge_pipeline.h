// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>
#include <vector>

#include "bridge/events.h"
#include "bridge/frame_assembler.h"
#include "bridge/line_splitter.h"

namespace curia::bridge {

// bytes -> LineSplitter -> parseLine -> FrameAssembler in one call chain. Still pure: the caller
// (the log watcher, a test, the fuzz target) supplies the bytes.
class BridgePipeline {
public:
    // Appends log bytes; returns the events of the lines they completed, in order.
    std::vector<Event> feed(std::string_view bytes);

    // The log was truncated: drops the unterminated tail and abandons any open frame.
    std::vector<Event> truncateLog();

    // One already split line (what feed() does per line).
    std::vector<Event> feedLine(const Line& line);

    const FrameAssembler& assembler() const noexcept { return assembler_; }
    const LineSplitter& splitter() const noexcept { return splitter_; }

private:
    LineSplitter splitter_;
    FrameAssembler assembler_;
};

}  // namespace curia::bridge
