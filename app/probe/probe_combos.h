// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace curia::probe {

// Collection-behaviour bits tried in the M2 experiment E3. The macOS layer maps them to
// NSWindowCollectionBehavior; this file stays portable so the table can be unit-tested.
constexpr unsigned kJoinAllSpaces = 1U << 0U;
constexpr unsigned kFullScreenAuxiliary = 1U << 1U;
constexpr unsigned kStationary = 1U << 2U;
constexpr unsigned kJoinAllApplications = 1U << 3U;

// One window level plus one flag set. `number` is 1-based and is what the probe window shows and
// the log records, so the owner can note "combination 7" instead of switching settings by hand.
struct Combo {
    int number;
    int level;  // NSWindowLevel value
    const char* levelName;
    char flagSet;  // 'A'..'E'
    unsigned flags;
};

// Levels (outer loop, most likely to work first) times flag sets (inner loop).
const std::vector<Combo>& allCombos();

// nullptr when `number` is outside 1..allCombos().size().
const Combo* comboByNumber(int number);

// "canJoinAllSpaces|fullScreenAuxiliary|stationary", or "none".
std::string flagNames(unsigned flags);

// "combo 1/20 level 1000 (screenSaver) flags A canJoinAllSpaces|fullScreenAuxiliary|stationary"
std::string describeCombo(const Combo& combo);

// 0-based index of the combination that is active `elapsedSeconds` after the cycle started, or -1
// before the start, after the last step, or when the step length is not positive.
int comboIndexAt(double elapsedSeconds, double stepSeconds, int comboCount);

}  // namespace curia::probe
