// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>

// Rectangles and points on the desktop, in one coordinate system for every OS: global, with the
// origin at the top-left corner of the primary display and y growing downwards (as CoreGraphics'
// window list, Windows and X11 count). macOS's NSScreen frames count from the bottom-left corner
// with y growing upwards; platform/placement.h converts between the two. The unit is whatever the
// OS gives: points on macOS, physical pixels on Windows (GetWindowRect of a DPI-aware process).
namespace curia::platform {

struct ScreenPoint {
    double x = 0;
    double y = 0;

    friend bool operator==(const ScreenPoint& a, const ScreenPoint& b) {
        return a.x == b.x && a.y == b.y;
    }
};

struct ScreenRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;

    [[nodiscard]] double right() const { return x + width; }
    [[nodiscard]] double bottom() const { return y + height; }
    [[nodiscard]] bool empty() const { return !(width > 0) || !(height > 0); }
    [[nodiscard]] double area() const { return empty() ? 0.0 : width * height; }
    // The right and bottom edges are outside (a point on the edge of two displays belongs to one).
    [[nodiscard]] bool contains(ScreenPoint p) const {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }

    friend bool operator==(const ScreenRect& a, const ScreenRect& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
};

// The overlap of two rectangles; an empty rectangle (width or height 0) when they do not overlap.
inline ScreenRect intersect(const ScreenRect& a, const ScreenRect& b) {
    const double left = std::max(a.x, b.x);
    const double top = std::max(a.y, b.y);
    const double right = std::min(a.right(), b.right());
    const double bottom = std::min(a.bottom(), b.bottom());
    if (right <= left || bottom <= top) {
        return ScreenRect{left, top, 0, 0};
    }
    return ScreenRect{left, top, right - left, bottom - top};
}

}  // namespace curia::platform
