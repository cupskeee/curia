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

    // Plain words only (plain_words.h): the model repeats them to the player.
    std::string realm;
    if (freshness.gameDateOfSnapshot.has_value()) {
        realm = "the realm information is from game date " +
                formatGameDate(*freshness.gameDateOfSnapshot);
    }
    if (old) {
        realm += (realm.empty() ? "the realm information was read " : " and was read ") +
                 describeDuration(freshness.snapshotAgeSeconds) + " ago (real time)";
    }
    if (!realm.empty()) {
        parts.push_back(realm);
    }
    if (silent) {
        parts.push_back("no update has come from the game for " +
                        describeDuration(freshness.logSilentSeconds));
    }
    if (freshness.exportUnreadable) {
        parts.emplace_back(
            "the latest update from the game could not be read, so newer information may exist");
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
