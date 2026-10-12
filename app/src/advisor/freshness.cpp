// SPDX-License-Identifier: MIT
#include "advisor/freshness.h"

#include <vector>

#include "advisor/snapshot_renderer.h"

namespace curia::advisor {

std::string describeDuration(long long seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    const long long minutes = seconds / 60;
    if (minutes < 1) {
        return "less than a minute";
    }
    constexpr long long kMinutesPerHour = 60;
    constexpr long long kHoursFromMinutes = 120;
    if (minutes < kHoursFromMinutes) {
        return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
    }
    const long long hours = minutes / kMinutesPerHour;
    return std::to_string(hours) + " hours";
}

std::string freshnessNote(const Freshness& freshness) {
    const bool old = freshness.snapshotAgeSeconds >= kSnapshotOldAfterSeconds;
    const bool silent = freshness.logSilentSeconds >= kLogSilentAfterSeconds;
    if (!old && !silent && !freshness.exportUnreadable) {
        return {};
    }
    std::vector<std::string> parts;

    std::string snapshot;
    if (freshness.gameDateOfSnapshot.has_value()) {
        snapshot =
            "the snapshot is from game date " + formatGameDate(*freshness.gameDateOfSnapshot);
    }
    if (old) {
        snapshot += (snapshot.empty() ? "the snapshot was taken " : " and was taken ") +
                    describeDuration(freshness.snapshotAgeSeconds) + " ago (real time)";
    }
    if (!snapshot.empty()) {
        parts.push_back(snapshot);
    }
    if (silent) {
        parts.push_back("Curia has seen no new game data for " +
                        describeDuration(freshness.logSilentSeconds));
    }
    if (freshness.exportUnreadable) {
        parts.emplace_back("the last export could not be read, so newer game data may exist");
    }

    std::string note = "[Context: ";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            note += "; ";
        }
        note += parts[i];
    }
    note += ".]";
    return note;
}

}  // namespace curia::advisor
