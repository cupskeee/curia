// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "bridge/events.h"
#include "bridge/frame_assembler.h"
#include "bridge/line_splitter.h"

namespace curia::bridge {

// bytes -> LineSplitter -> parseLine -> FrameAssembler in one call chain. Still pure: the caller
// (the log watcher, a test, the fuzz target) supplies the bytes.
//
// Memory: lines are parsed and assembled as they complete, so the work in flight is bounded by the
// longest line (4,096 bytes) and the open frame (512 records, 128 KiB). The events returned grow
// with the input, though: each completed frame is an event. The watcher should therefore feed the
// log in chunks of at most about 64 KiB, also for the first catch-up read of a large file.
class BridgePipeline {
public:
    // At most this many Diagnostic events are delivered between two frame events (Complete,
    // Damaged, unsupported format); the rest are only counted (suppressedDiagnostics()). A log full
    // of invalid Curia-looking lines cannot make one event per line. Chunk-independent.
    static constexpr std::size_t kMaxDiagnosticsInARun = 64;

    // Appends log bytes; returns the events of the lines they completed, in order.
    std::vector<Event> feed(std::string_view bytes);

    // The log was truncated: drops the unterminated tail and abandons any open frame.
    std::vector<Event> truncateLog();

    // One already split line (what feed() does per line).
    std::vector<Event> feedLine(const Line& line);

    const FrameAssembler& assembler() const noexcept { return assembler_; }
    const LineSplitter& splitter() const noexcept { return splitter_; }

    // Diagnostics dropped by the cap above since this object was created.
    std::uint64_t suppressedDiagnostics() const noexcept { return suppressedDiagnostics_; }

private:
    LineSplitter splitter_;
    FrameAssembler assembler_;
    std::size_t diagnosticRun_ = 0;
    std::uint64_t suppressedDiagnostics_ = 0;

    void feedLineInto(const Line& line, std::vector<Event>& events);
    void collect(std::vector<Event>&& from, std::vector<Event>& to);
};

}  // namespace curia::bridge
