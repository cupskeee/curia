// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "advisor/freshness.h"
#include "bridge/frame.h"
#include "core/clock.h"

namespace curia::advisor {

// The newest Complete frame, when it arrived (injected clock), its rendered text, and whether the
// export is currently unreadable (docs/wire-format.md section 3: arrival order is authoritative; a
// Damaged event never replaces the stored frame, it only sets a flag that the next Complete frame
// clears). The chat transcript is NOT kept here: it lives in ChatSession, which a new snapshot
// leaves alone (the controller only adds a divider).
//
// Not thread-safe: it belongs to the UI thread, like the rest of the advisor layer.
class SnapshotStore {
public:
    explicit SnapshotStore(ClockFn clock);

    struct Update {
        // The new frame is the first one this store has seen.
        bool first = false;
        // Its date is earlier than the previous Complete frame's: a hint that a save was loaded
        // or a new game started (equal dates are not a hint).
        bool dateWentBack = false;
    };

    Update onFrameComplete(bridge::Frame frame);
    // A damaged frame, or a frame of an unsupported mod format.
    void onExportUnreadable();

    bool hasSnapshot() const { return frame_.has_value(); }
    const bridge::Frame* frame() const { return frame_ ? &*frame_ : nullptr; }
    // The rendered block, or renderNoSnapshot() while there is no frame.
    const std::string& text() const { return text_; }
    bool unreadable() const { return unreadable_; }
    // Number of Complete frames received (0 = none yet).
    std::uint64_t sequence() const { return sequence_; }
    std::optional<TimePoint> arrivedAt() const { return arrivedAt_; }

    // The inputs of the freshness note at `now`. `logSilentSeconds` comes from the log monitor.
    Freshness freshness(TimePoint now, long long logSilentSeconds) const;

private:
    ClockFn clock_;
    std::optional<bridge::Frame> frame_;
    std::string text_;
    bool unreadable_ = false;
    std::uint64_t sequence_ = 0;
    std::optional<TimePoint> arrivedAt_;
};

}  // namespace curia::advisor
