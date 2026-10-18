// SPDX-License-Identifier: MIT
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>

#include "core/cli.h"

namespace curia {
class AppLog;
}  // namespace curia

namespace curia::platform {
class Platform;
}  // namespace curia::platform

// The window the chat is drawn in, with the two ways of showing and hiding it (FR-OVL-1/2/8/9):
//   * the overlay panel (macOS, ui/overlay_mac.mm): our own borderless, non-activating NSPanel at
//     the screen-saver level, wrapped into an SDL window, under the Accessory activation policy:
//     the recipe that M2 found visible over CK3's Fullscreen Space (docs/spikes/m2-results.md). It
//     is placed on CK3's display every time it is shown, shown without activating Curia, and
//     hidden with orderOut, after which the keys go back to CK3;
//   * the plain window (Windows, Linux, and the macOS fallback --window plain,
//     ui/overlay_plain.cpp): a normal resizable SDL window that stays on top of other windows (in
//     the chat window, not in the test modes). Shown = restored if minimised and raised; hidden =
//     minimised. It is not moved.
// UI thread only (SDL and AppKit).
namespace curia::ui {

// Why the overlay is shown (the app log says so).
enum class ShowReason : std::uint8_t { Start, GameClick, Hotkey, Tray };

const char* toString(ShowReason reason) noexcept;

struct OverlayOptions {
    // The size in logical pixels (points on macOS); the plain window multiplies it by the primary
    // display's content scale where the OS counts window sizes in pixels.
    int width = 520;
    int height = 680;
    // The smallest the panel shrinks to on a small CK3 window, and the plain window's minimum size.
    int minWidth = 340;
    int minHeight = 420;
    // The panel only: who gets the keys when it is shown (core/cli.h).
    OverlayFocus focus = OverlayFocus::Key;
    // The panel only: place it on CK3's window when one is found (FR-OVL-9). False: always on the
    // pointer's display (the test modes, which must not depend on what runs on the machine).
    bool followGame = true;
    // The plain window only: SDL_WINDOW_ALWAYS_ON_TOP (the chat window; not the test modes).
    bool alwaysOnTop = false;
    // Not owned; may be null (then the panel never finds CK3's window).
    const platform::Platform* platform = nullptr;
    // Not owned; may be null. Gets short words and numbers only.
    AppLog* log = nullptr;
};

class Overlay {
public:
    virtual ~Overlay() = default;
    Overlay() = default;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // The SDL window the renderer draws into. Owned by the overlay (destroyed with it, after the
    // renderer: destroy the render backend first).
    virtual SDL_Window* window() const = 0;
    // Places (the panel, on every show), puts it in front and hands out the keys by the focus
    // mode. The show at start-up (ShowReason::Start) only puts it in front, whatever the mode: the
    // player has not asked to type yet. The run loop then focuses the input box and draws.
    virtual void show(ShowReason reason) = 0;
    // Hides it. A running answer keeps arriving in the background (FR-OVL-4). Whether the keys
    // went back to the game is settled later (settle()).
    virtual void hide() = 0;
    // The run loop calls this a short while (kSettleMs in app.cpp) after the last show() or
    // hide(): the panel logs the focus as it settled (overlay_focus), and after a hide or the
    // start-up show, if Curia is the active app, it asks another app to activate so that the keys
    // go back to the game or to the app Curia was started from (FR-OVL-2; after a hide M2's plain
    // orderOut needed no help, while the launch can activate Curia). True = it asked an app to
    // activate: call settle() once more a short while later, which logs the result.
    virtual bool settle() = 0;
    // Logs the focus now (overlay_focus why=<why>): shown or not, whether the panel is the key
    // window and Curia the active app (the plain window: whether it has the keyboard), whether CK3
    // is in front. Short words only, never a key or a text. Nothing without an app log.
    virtual void logFocus(const char* why) const = 0;
    // Shown and not minimised (whatever SDL's flags say: orderOut and orderFrontRegardless reach
    // SDL only through its observation of the window).
    virtual bool visible() const = 0;
    // "panel" or "window", for the app log.
    virtual const char* kind() const = 0;
};

// Before SDL_Init: for the panel, the hints that keep SDL from applying the Regular activation
// policy and from activating Curia when a window is shown or raised (SDL_HINT_MAC_BACKGROUND_APP,
// SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN/RAISED). Nothing for the plain window.
void prepareOverlayBeforeInit(WindowKind kind);

// After SDL_Init. Null on failure (SDL_GetError() says why) and for WindowKind::Overlay where the
// panel does not exist. The panel also sets the Accessory activation policy (no Dock icon; the
// hint alone left Regular in M2).
std::unique_ptr<Overlay> createOverlay(WindowKind kind, const OverlayOptions& options);

// The two implementations (createOverlay picks one).
std::unique_ptr<Overlay> createPlainOverlay(const OverlayOptions& options);
std::unique_ptr<Overlay> createPanelOverlay(const OverlayOptions& options);

}  // namespace curia::ui
