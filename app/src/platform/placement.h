// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "platform/screen_rect.h"

// Where the overlay goes when it is shown (FR-OVL-9): pure arithmetic over rectangles that the
// platform code collects, so that every case (two displays, a display left of or above the primary
// one, CK3's window across two displays, a small window) is unit-tested on any machine.
namespace curia::platform {

// One display, in the top-left-origin coordinates of platform/screen_rect.h.
struct DisplayArea {
    ScreenRect frame;
    // The part windows may use (macOS: without the menu bar and the Dock, NSScreen.visibleFrame).
    // An empty rectangle means the whole frame.
    ScreenRect visible;
};

struct PlacementRequest {
    // The displays with the primary one first (on macOS NSScreen.screens[0], the display whose
    // frame starts at 0,0).
    std::vector<DisplayArea> displays;
    // CK3's window (Platform::findGameWindow()), if one was found.
    std::optional<ScreenRect> gameWindow;
    // The mouse pointer, if known.
    std::optional<ScreenPoint> pointer;
    // The overlay's preferred size, the smallest it may shrink to, and the gap it keeps from the
    // edges of CK3's window (or of the display).
    double width = 0;
    double height = 0;
    double minWidth = 0;
    double minHeight = 0;
    double margin = 0;
};

// What the placement went by.
enum class PlacementBasis : std::uint8_t {
    // The display with the largest part of CK3's window; the overlay sits on that part.
    GameWindow,
    // No CK3 window (or none on a display): the display under the pointer.
    Pointer,
    // Neither: the primary display.
    Primary,
};

[[nodiscard]] const char* toString(PlacementBasis basis) noexcept;

struct Placement {
    ScreenRect frame;
    // Index into PlacementRequest::displays.
    std::size_t display = 0;
    PlacementBasis basis = PlacementBasis::Primary;
};

// The rule: pick the display (above); the area is the part of CK3's window on that display's
// visible area (or the visible area itself without a CK3 window). The overlay keeps its preferred
// size where the area has room for it and the margins, shrinks to fit a smaller area but never
// below its minimum (nor below a preferred size that is smaller than the minimum, nor beyond the
// display's visible area), and sits at the right edge of the area, `margin` in from it, centred
// from top to bottom; it is then kept inside the visible area. Whole units. std::nullopt only when
// there is no display.
[[nodiscard]] std::optional<Placement> placeOverlay(const PlacementRequest& request);

// macOS frames (NSScreen, NSWindow) count from the bottom-left corner of the primary display with
// y growing upwards; CoreGraphics' window list and these functions count from its top-left corner
// with y growing downwards. `primaryHeight` is the height of the primary display's frame. The
// conversion is its own inverse; both names exist so that the call sites say which way they go.
[[nodiscard]] ScreenRect cocoaToTopLeft(const ScreenRect& cocoa, double primaryHeight);
[[nodiscard]] ScreenRect topLeftToCocoa(const ScreenRect& topLeft, double primaryHeight);
[[nodiscard]] ScreenPoint cocoaToTopLeft(ScreenPoint cocoa, double primaryHeight);

}  // namespace curia::platform
