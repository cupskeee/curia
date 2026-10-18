// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "platform/screen_rect.h"

// The pure decisions behind Platform::isGameRunning() and Platform::findGameWindow(): which process
// is CK3 and which of its windows is the game. Each OS collects the raw facts (process names, the
// window list) and leaves the choice to these functions, so the rules are unit-tested everywhere.
namespace curia::platform {

// CK3's process: the executable "ck3" (macOS, native Linux) or "ck3.exe" (Windows, and Proton on
// Linux, whose process name is the Windows executable's), compared without regard to ASCII case.
// The macOS bundle id is empty (M2), so the name is what identifies the game.
[[nodiscard]] bool isGameProcessName(std::string_view name);

// One top-level window from the OS's window list.
struct WindowCandidate {
    long long pid = 0;
    // macOS: kCGWindowLayer (the game's own windows are at layer 0; menus, the Dock and overlays
    // are above). Windows: always 0.
    int layer = 0;
    ScreenRect rect;
};

// The game's window: among the candidates owned by one of `gamePids`, at layer 0 and with a
// non-empty rectangle, the one with the largest area (the rule M2 verified: on the owner's Mac
// CK3's largest layer-0 window was 0,39,2056,1290 on a 2056x1329 display, beside thin strips of
// CK3-owned windows on the other display, E9). Whether a window is on screen plays no part: M2 did
// not record it for the strips. Ties go to the earlier candidate (the window list is front to
// back). Nothing found = std::nullopt.
[[nodiscard]] std::optional<ScreenRect> pickGameWindow(const std::vector<WindowCandidate>& windows,
                                                       const std::vector<long long>& gamePids);

}  // namespace curia::platform
