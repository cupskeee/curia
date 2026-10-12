// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <optional>

#include "core/clock.h"

namespace curia::advisor {

// What the player should be told about a silent game log (decision of the M4 plan, two detectors).
enum class Notice {
    None,
    // The player asked for fresh data (opened the overlay by hot key, tray or Refresh while the
    // snapshot was old) and no frame arrived in time: the export may be dead until CK3 restarts.
    // Only a hint: a frame exists only after the player clicks Advisor in the game, so a healthy
    // CK3 gets this too if the player has not clicked yet. The wording shown must hedge ("No new
    // game data yet. Click Advisor in the game; if that does nothing, restart CK3").
    RestartCk3,
    // The log grew earlier and has been silent for a long time while CK3 runs: a soft notice.
    SilentSoft,
    // CK3 is not running.
    Ck3NotRunning,
};

const char* toString(Notice notice) noexcept;

// The values are in milliseconds so that the headless end-to-end tests can use thresholds of a
// fraction of a second; the app's defaults are 120 s, 15 s and 10 minutes.
struct LogSilenceConfig {
    // Armed detector: the snapshot must be at least this old when the overlay is opened by the
    // player (a younger one is fresh enough) ...
    std::chrono::milliseconds armSnapshotAge{120000};
    // ... and no frame may arrive within this time of the opening.
    std::chrono::milliseconds armWait{15000};
    // Passive detector: the log grew earlier and has been silent for this long.
    std::chrono::milliseconds passiveSilence{600000};
};

// A pure state machine over an injected clock: the inputs say what happened, notice() says what to
// show. It never reads the clock on its own except through the function it was given, and it
// starts no thread. The UI thread calls it; the app polls tick() about once a second.
//
// Rules:
//   * Ck3NotRunning wins over everything: while ck3Running(false) is the last word, the notice is
//     Ck3NotRunning (an unknown state, the default, counts as running: a platform that cannot
//     tell must not claim CK3 is closed). Learning that CK3 is not running also forgets a pending
//     armed window, an earlier RestartCk3 and the last log growth, so that the notice a player
//     gets after starting CK3 again is not left over from the previous run;
//   * armed detector: overlayOpenedByUser() arms it when no frame has ever arrived or the last one
//     is at least armSnapshotAge old. After armWait without a frame (and without frameDamaged())
//     the notice becomes RestartCk3, and stays until a frame arrives. A frame that arrives in the
//     window disarms it. The detector is blind to unrelated log growth on purpose: CK3 may write
//     nothing else while idle (T8 has not measured it), so growth alone proves nothing;
//   * passive detector: after the log has grown at least once, silence of passiveSilence while
//     CK3 runs gives SilentSoft; any growth or frame clears it;
//   * RestartCk3 beats SilentSoft.
class LogSilenceMonitor {
public:
    explicit LogSilenceMonitor(ClockFn clock, LogSilenceConfig config = {});

    // The game's log file got new bytes (any bytes).
    void logGrew();
    // A Complete frame arrived.
    void frameArrived();
    // A frame arrived but was damaged or unsupported: the export works but the data is not usable.
    // Disarms the armed detector; it does not count as a snapshot.
    void frameDamaged();
    // The overlay was opened by the player (hot key, tray, Refresh). Not for the opening the game
    // click itself causes.
    void overlayOpenedByUser();
    void ck3Running(bool running);
    // Re-evaluates at `now` (the injected clock is used by the input methods).
    void tick(TimePoint now);

    Notice notice() const noexcept { return notice_; }

    // Whole seconds since the log last grew at `now`; nullopt if it never did.
    std::optional<long long> secondsSinceLogGrew(TimePoint now) const;

private:
    void evaluate(TimePoint now);

    ClockFn clock_;
    LogSilenceConfig config_;
    std::optional<TimePoint> lastGrowth_;
    std::optional<TimePoint> lastFrame_;
    std::optional<TimePoint> armedAt_;
    bool restartNeeded_ = false;
    std::optional<bool> running_;
    Notice notice_ = Notice::None;
};

}  // namespace curia::advisor
