// SPDX-License-Identifier: MIT
#include "platform/placement.h"

#include <algorithm>
#include <cmath>

namespace curia::platform {

namespace {

ScreenRect usable(const DisplayArea& display) {
    return display.visible.empty() ? display.frame : display.visible;
}

// The size along one axis: the preferred size where the room allows it, else as much as there is
// room for, but at least the minimum (a preferred size below the minimum is its own minimum: an
// explicit --window-size wins), and never more than the display has.
double fit(double preferred, double minimum, double room, double display) {
    double size = std::min(preferred, room);
    size = std::max(size, std::min(minimum, preferred));
    return std::max(0.0, std::min(size, display));
}

// `start` moved so that [start, start + size) lies inside [low, high) where it fits.
double keepInside(double start, double size, double low, double high) {
    return std::max(low, std::min(start, high - size));
}

}  // namespace

const char* toString(PlacementBasis basis) noexcept {
    switch (basis) {
    case PlacementBasis::GameWindow:
        return "game";
    case PlacementBasis::Pointer:
        return "pointer";
    case PlacementBasis::Primary:
        return "primary";
    }
    return "primary";
}

std::optional<Placement> placeOverlay(const PlacementRequest& request) {
    if (request.displays.empty()) {
        return std::nullopt;
    }
    Placement placement;
    ScreenRect area;
    bool chosen = false;

    if (request.gameWindow.has_value() && !request.gameWindow->empty()) {
        double bestOverlap = 0;
        for (std::size_t i = 0; i < request.displays.size(); ++i) {
            const double overlap = intersect(*request.gameWindow, request.displays[i].frame).area();
            if (overlap > bestOverlap) {
                bestOverlap = overlap;
                placement.display = i;
                chosen = true;
            }
        }
        if (chosen) {
            placement.basis = PlacementBasis::GameWindow;
            const DisplayArea& display = request.displays[placement.display];
            area = intersect(*request.gameWindow, usable(display));
            if (area.empty()) {
                area = usable(display);  // CK3's part lies under the menu bar or the Dock only
            }
        }
    }
    if (!chosen && request.pointer.has_value()) {
        for (std::size_t i = 0; i < request.displays.size(); ++i) {
            if (request.displays[i].frame.contains(*request.pointer)) {
                placement.display = i;
                placement.basis = PlacementBasis::Pointer;
                area = usable(request.displays[i]);
                chosen = true;
                break;
            }
        }
    }
    if (!chosen) {
        placement.display = 0;
        placement.basis = PlacementBasis::Primary;
        area = usable(request.displays[0]);
    }

    const ScreenRect visible = usable(request.displays[placement.display]);
    const double margin = std::max(0.0, request.margin);
    const double width =
        std::round(fit(request.width, request.minWidth, area.width - 2.0 * margin, visible.width));
    const double height = std::round(
        fit(request.height, request.minHeight, area.height - 2.0 * margin, visible.height));
    const double x = keepInside(area.right() - margin - width, width, visible.x, visible.right());
    const double y =
        keepInside(area.y + (area.height - height) / 2.0, height, visible.y, visible.bottom());
    placement.frame = ScreenRect{std::round(x), std::round(y), width, height};
    return placement;
}

ScreenRect cocoaToTopLeft(const ScreenRect& cocoa, double primaryHeight) {
    return ScreenRect{cocoa.x, primaryHeight - (cocoa.y + cocoa.height), cocoa.width, cocoa.height};
}

ScreenRect topLeftToCocoa(const ScreenRect& topLeft, double primaryHeight) {
    return cocoaToTopLeft(topLeft, primaryHeight);
}

ScreenPoint cocoaToTopLeft(ScreenPoint cocoa, double primaryHeight) {
    return ScreenPoint{cocoa.x, primaryHeight - cocoa.y};
}

}  // namespace curia::platform
