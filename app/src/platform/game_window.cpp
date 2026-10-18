// SPDX-License-Identifier: MIT
#include "platform/game_window.h"

#include <algorithm>

namespace curia::platform {

namespace {

char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool isGameProcessName(std::string_view name) {
    return equalsIgnoringCase(name, "ck3") || equalsIgnoringCase(name, "ck3.exe");
}

std::optional<ScreenRect> pickGameWindow(const std::vector<WindowCandidate>& windows,
                                         const std::vector<long long>& gamePids) {
    const WindowCandidate* best = nullptr;
    for (const WindowCandidate& window : windows) {
        const bool ownedByGame =
            std::find(gamePids.begin(), gamePids.end(), window.pid) != gamePids.end();
        if (!ownedByGame || window.layer != 0 || window.rect.empty()) {
            continue;
        }
        // The larger area wins; ties keep the earlier one.
        if (best == nullptr || window.rect.area() > best->rect.area()) {
            best = &window;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return best->rect;
}

}  // namespace curia::platform
