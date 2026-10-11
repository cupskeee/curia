// SPDX-License-Identifier: MIT
#pragma once

#include <ostream>

#include "core/cli.h"

namespace curia {

inline constexpr int kDefaultWatchTimeoutSeconds = 60;

// The headless `curia --watch-log` mode: no SDL, no window. Tails options.watchLogPath with a
// LogWatcher (250 ms polls) and writes one JSON object per bridge event to `out`, one per line and
// flushed per line (written from the watcher thread; the streams are not touched by the caller
// until this returns) (the format of bridge/frame_json.h). Progress notes go to `err`:
//   curia: log attached | truncated | missing | unreadable | recovered
//   curia: offset <n> total <bytes> curia <bytes>     (after each poll that read bytes)
// The end-to-end test with the fake CK3 reads these to know how far the tail has got.
//
// Returns 0 after options.maxFrames complete frames, or, when no frame count was asked for, at the
// timeout (options.timeoutSeconds, default kDefaultWatchTimeoutSeconds); 3 when the timeout comes
// before the asked-for frames.
int runWatchLog(const CliOptions& options, std::ostream& out, std::ostream& err);

}  // namespace curia
