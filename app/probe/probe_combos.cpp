// SPDX-License-Identifier: MIT
#include "probe_combos.h"

#include <array>
#include <cstddef>

namespace curia::probe {

namespace {

struct LevelSpec {
    int value;
    const char* name;
};

struct FlagSpec {
    char letter;
    unsigned bits;
};

// Window levels from the macOS SDK: floating 3, status 25, pop-up menu 101, screen saver 1000.
constexpr std::array<LevelSpec, 4> kLevels = {{
    {1000, "screenSaver"},
    {101, "popUpMenu"},
    {25, "status"},
    {3, "floating"},
}};

// A is the Apple DTS recipe without canJoinAllApplications; B is the DTS sample as published.
// C drops stationary, D drops canJoinAllSpaces and fullScreenAuxiliary (is canJoinAllApplications
// enough?), E drops fullScreenAuxiliary (is it needed next to canJoinAllSpaces?).
constexpr std::array<FlagSpec, 5> kFlagSets = {{
    {'A', kJoinAllSpaces | kFullScreenAuxiliary | kStationary},
    {'B', kJoinAllSpaces | kFullScreenAuxiliary | kStationary | kJoinAllApplications},
    {'C', kJoinAllSpaces | kFullScreenAuxiliary},
    {'D', kJoinAllApplications | kStationary},
    {'E', kJoinAllSpaces | kStationary},
}};

std::vector<Combo> buildCombos() {
    std::vector<Combo> out;
    int number = 1;
    for (const auto& level : kLevels) {
        for (const auto& set : kFlagSets) {
            out.push_back(Combo{number++, level.value, level.name, set.letter, set.bits});
        }
    }
    return out;
}

}  // namespace

const std::vector<Combo>& allCombos() {
    static const std::vector<Combo> combos = buildCombos();
    return combos;
}

const Combo* comboByNumber(int number) {
    const auto& combos = allCombos();
    if (number < 1 || static_cast<std::size_t>(number) > combos.size()) {
        return nullptr;
    }
    return &combos[static_cast<std::size_t>(number) - 1U];
}

std::string flagNames(unsigned flags) {
    struct Named {
        unsigned bit;
        const char* name;
    };
    static constexpr std::array<Named, 4> kNames = {{
        {kJoinAllSpaces, "canJoinAllSpaces"},
        {kFullScreenAuxiliary, "fullScreenAuxiliary"},
        {kStationary, "stationary"},
        {kJoinAllApplications, "canJoinAllApplications"},
    }};
    std::string out;
    for (const auto& named : kNames) {
        if ((flags & named.bit) != 0U) {
            if (!out.empty()) {
                out += '|';
            }
            out += named.name;
        }
    }
    return out.empty() ? "none" : out;
}

std::string describeCombo(const Combo& combo) {
    return "combo " + std::to_string(combo.number) + "/" + std::to_string(allCombos().size()) +
           " level " + std::to_string(combo.level) + " (" + combo.levelName + ") flags " +
           combo.flagSet + " " + flagNames(combo.flags);
}

int comboIndexAt(double elapsedSeconds, double stepSeconds, int comboCount) {
    if (stepSeconds <= 0.0 || elapsedSeconds < 0.0 || comboCount <= 0) {
        return -1;
    }
    const auto index = static_cast<int>(elapsedSeconds / stepSeconds);
    return index < comboCount ? index : -1;
}

}  // namespace curia::probe
