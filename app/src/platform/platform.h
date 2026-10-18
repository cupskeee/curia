// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "platform/screen_rect.h"

namespace curia::platform {

// Thin per-OS layer. Shared code never includes platform headers; each OS implements this interface
// in platform/{win,mac,linux} and CMake picks one. The calls here need no window (they are part of
// curia_core and unit-tested); the overlay window, the hot key and the tray belong to the curia
// executable (app/src/ui/overlay.h, hotkey.h, tray.h). Secret storage and process spawning join
// in later milestones (docs/architecture.md section 5.3). UI thread only.
class Platform {
public:
    virtual ~Platform() = default;

    virtual std::string_view name() const = 0;

    // This process's id (the app log names it, so that the lines of two instances can be told
    // apart).
    [[nodiscard]] virtual long long processId() const = 0;

    // Places where CK3's user folder (the one containing logs/debug.log) may be, most likely first.
    virtual std::vector<std::filesystem::path> candidateCk3UserDirs() const = 0;

    // --- the game (platform/game_window.h has the rules) -------------------------------------
    // Whether a CK3 process is running: macOS, a running application whose executable is ck3
    // (NSWorkspace); Windows, a process ck3.exe (a process snapshot); Linux, a process whose
    // /proc/<pid>/comm is ck3 or ck3.exe (Proton). std::nullopt = this OS cannot tell now.
    [[nodiscard]] virtual std::optional<bool> isGameRunning() const = 0;
    // Whether CK3 is the frontmost application: macOS, NSWorkspace's frontmost application (the
    // design choice of M2: NSWorkspace still reports CK3 while the overlay is the key window);
    // Windows, the process of the foreground window. std::nullopt = unknown (Linux in M4).
    [[nodiscard]] virtual std::optional<bool> gameIsFrontmost() const = 0;
    // CK3's window in the coordinates of platform/screen_rect.h: macOS, the largest layer-0 window
    // of the ck3 process from CGWindowListCopyWindowInfo, in points (bounds, layer and owner come
    // back without Screen Recording and without a prompt, M2; window titles do not, and nothing
    // here reads them); Windows, the largest visible top-level window of ck3.exe from EnumWindows
    // and GetWindowRect, in physical pixels once SDL's video subsystem has started (SDL 3.4
    // declares the process per-monitor DPI aware there). std::nullopt = no window, or the OS cannot
    // tell (Linux in M4).
    [[nodiscard]] virtual std::optional<ScreenRect> findGameWindow() const = 0;
    // isGameRunning() is asked on every n-th tick of the window's once-a-second timer (1 = every
    // second; Linux reads every /proc/<pid>/comm and asks less often).
    [[nodiscard]] virtual int gameRunningPollTicks() const { return 1; }
};

std::unique_ptr<Platform> create();

}  // namespace curia::platform
