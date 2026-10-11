// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bridge/events.h"
#include "bridge/record.h"

namespace curia::bridge {

// The frame state machine of docs/wire-format.md section 3. Pure: parse results in, events out.
// Idle and Damaged ignore everything except a BEGIN (and a truncation); Open stores the frame's
// records and checks them. Exactly one FrameDamaged is emitted per frame that fails.
class FrameAssembler {
public:
    enum class State { Idle, Open, Damaged };

    // The 513th frame record, or frame-record bytes above 128 KiB, make a frame too large.
    static constexpr std::size_t kMaxFrameRecords = 512;
    static constexpr std::size_t kMaxFrameBytes = 128 * 1024;

    // Feeds the outcome of parseLine(); NotCuria results are ignored.
    std::vector<Event> consume(const ParseResult& result);

    // The log was truncated (the unterminated tail is the splitter's concern).
    std::vector<Event> truncateLog();

    State state() const noexcept { return state_; }

private:
    void handleInvalid(const InvalidRecord& invalid, std::vector<Event>& events);
    void handleRecord(const Record& record, std::vector<Event>& events);
    void openFrame(const Record& begin);
    void damage(DamageReason reason, std::string detail, std::vector<Event>& events);
    void finish(const Record& end, std::vector<Event>& events);
    void reset();

    State state_ = State::Idle;
    std::int32_t snap_ = 0;
    // Frame records seen so far, BEGIN included (and END, once it arrives).
    std::size_t count_ = 0;
    std::size_t bytes_ = 0;
    bool shortened_ = false;
    // The known-type records of the open frame, BEGIN first. Unknown types are only counted.
    std::vector<Record> records_;
};

}  // namespace curia::bridge
