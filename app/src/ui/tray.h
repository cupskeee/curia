// SPDX-License-Identifier: MIT
#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string_view>

// The tray icon and its menu (FR-OVL-6): "Show Curia", "Hide Curia", "Quit Curia". Secondary: on
// macOS the menu bar did not appear on CK3's Fullscreen display (M2), so the in-game button and the
// hot key must work without it. SDL_CreateTray activates the app on macOS (M2, the same
// millisecond), so the tray is created once at start-up, before the main loop, and never later,
// and not at all when CK3 is in front then (app.cpp). Best effort: where SDL cannot make a tray
// Curia runs without one; Linux has none in M4 (SDL polls a tray every 50 ms there, which would
// break the idle rule NFR-1). The menu's callbacks only push one SDL event each (the entry in
// event.user.code). UI thread only.
namespace curia::ui {

class Tray {
public:
    enum Entry : Sint32 { Show = 1, Hide = 2, Quit = 3 };

    // Null when SDL cannot make a tray here (`status` "unavailable") and on Linux
    // ("skipped_linux"); else `status` is "created".
    static std::unique_ptr<Tray> create(Uint32 eventType, std::string_view& status);

    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

private:
    explicit Tray(SDL_Tray* tray) : tray_(tray) {}
    SDL_Tray* tray_ = nullptr;
};

}  // namespace curia::ui
