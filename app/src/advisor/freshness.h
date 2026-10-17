// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>

#include "bridge/frame.h"

// The "how old is this snapshot" note that goes at the end of the latest user message. The wording
// comes from a pure function of a few numbers so that it can be tested without a clock, and it
// stays out of both system blocks and out of the history, so that neither the persona nor the
// snapshot nor an earlier turn changes between two questions about the same snapshot (the
// provider's cache needs byte-identical prefixes).
namespace curia::advisor {

// A snapshot older than this (real time) is mentioned. Matches the armed detector's age of
// LogSilenceMonitor; tuned later with real data (T8).
inline constexpr long long kSnapshotOldAfterSeconds = 120;
// The log was silent at least this long (while Curia runs): mentioned.
inline constexpr long long kLogSilentAfterSeconds = 300;

struct Freshness {
    // Real seconds since the snapshot arrived.
    long long snapshotAgeSeconds = 0;
    // Real seconds since the game log last grew; 0 when it never did.
    long long logSilentSeconds = 0;
    // The last export could not be read (a damaged frame or an unsupported format).
    bool exportUnreadable = false;
    // The game date of the snapshot, if there is one.
    std::optional<bridge::Date> gameDateOfSnapshot;
};

// "less than a minute", "5 minutes", "1 minute", "3 hours" (whole units, rounded down).
std::string describeDuration(long long seconds);

// The note, or an empty string when everything is fresh (young snapshot, log not silent, export
// readable). It is in plain words (plain_words.h), because the model repeats it to the player.
// Example:
//   [Context: the realm information is from game date 15 September 1066 and was read 7 minutes
//   ago (real time); no update has come from the game for 6 minutes; the latest update from the
//   game could not be read, so newer information may exist.]
std::string freshnessNote(const Freshness& freshness);

}  // namespace curia::advisor
