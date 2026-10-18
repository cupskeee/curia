// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <optional>
#include <ostream>
#include <string_view>
#include <vector>

#include "platform/placement.h"

using curia::platform::DisplayArea;
using curia::platform::Placement;
using curia::platform::PlacementBasis;
using curia::platform::PlacementRequest;
using curia::platform::ScreenPoint;
using curia::platform::ScreenRect;

namespace curia::platform {

// For doctest's messages.
std::ostream& operator<<(std::ostream& out, const ScreenRect& rect) {
    return out << rect.x << ',' << rect.y << ',' << rect.width << 'x' << rect.height;
}

std::ostream& operator<<(std::ostream& out, const ScreenPoint& point) {
    return out << point.x << ',' << point.y;
}

}  // namespace curia::platform

namespace {

// The overlay of the app: 520x680 points, at least 340x420, 24 points from the edges.
PlacementRequest request(std::vector<DisplayArea> displays) {
    PlacementRequest r;
    r.displays = std::move(displays);
    r.width = 520;
    r.height = 680;
    r.minWidth = 340;
    r.minHeight = 420;
    r.margin = 24;
    return r;
}

DisplayArea display(ScreenRect frame, ScreenRect visible = {}) {
    return DisplayArea{frame, visible};
}

Placement place(const PlacementRequest& r) {
    const std::optional<Placement> placed = curia::platform::placeOverlay(r);
    REQUIRE(placed.has_value());
    return placed.value_or(Placement{});
}

}  // namespace

TEST_CASE("placement: one display, no CK3 window: the pointer's display, at its right edge") {
    PlacementRequest r = request({display({0, 0, 1440, 900}, {0, 24, 1440, 876})});
    r.pointer = ScreenPoint{100, 100};
    const Placement placed = place(r);
    CHECK((placed.basis == PlacementBasis::Pointer));
    CHECK(placed.display == 0);
    // Right edge of the visible area less the margin; centred between the menu bar and the bottom.
    CHECK(placed.frame == ScreenRect{896, 122, 520, 680});
}

TEST_CASE("placement: the owner's Mac (M2): CK3's window on the primary display, pointer on the "
          "other one") {
    // CK3's largest layer-0 window was 0,39,2056,1290 on a 2056x1329 display (M2, E9).
    PlacementRequest r =
        request({display({0, 0, 2056, 1329}, {0, 39, 2056, 1290}), display({2056, 0, 1920, 1080})});
    r.gameWindow = ScreenRect{0, 39, 2056, 1290};
    r.pointer = ScreenPoint{2500, 500};
    const Placement placed = place(r);
    CHECK((placed.basis == PlacementBasis::GameWindow));
    CHECK(placed.display == 0);
    CHECK(placed.frame == ScreenRect{1512, 344, 520, 680});
}

TEST_CASE("placement: two displays side by side, CK3 on the second") {
    PlacementRequest r = request(
        {display({0, 0, 1920, 1080}), display({1920, 0, 2560, 1440}, {1920, 25, 2560, 1414})});
    r.gameWindow = ScreenRect{1920, 25, 2560, 1414};
    r.pointer = ScreenPoint{10, 10};
    const Placement placed = place(r);
    CHECK((placed.basis == PlacementBasis::GameWindow));
    CHECK(placed.display == 1);
    // At the right edge less the margin, centred: 25 + (1414 - 680) / 2 = 392.
    CHECK(placed.frame == ScreenRect{1920 + 2560 - 24 - 520, 392, 520, 680});
}

TEST_CASE("placement: a display left of the primary one (negative x)") {
    PlacementRequest r = request({display({0, 0, 1440, 900}), display({-1920, 100, 1920, 1080})});
    r.gameWindow = ScreenRect{-1900, 120, 1600, 900};
    const Placement placed = place(r);
    CHECK(placed.display == 1);
    CHECK((placed.basis == PlacementBasis::GameWindow));
    CHECK(placed.frame == ScreenRect{-844, 230, 520, 680});
}

TEST_CASE("placement: a display above the primary one (negative y)") {
    PlacementRequest r = request({display({0, 0, 1440, 900}), display({-200, -1080, 1920, 1080})});
    r.gameWindow = ScreenRect{-200, -1080, 1920, 1080};
    r.pointer = ScreenPoint{700, 450};
    const Placement placed = place(r);
    CHECK(placed.display == 1);
    CHECK(placed.frame == ScreenRect{1176, -880, 520, 680});
    // Without CK3, the pointer up there picks that display too.
    r.gameWindow.reset();
    r.pointer = ScreenPoint{0, -10};
    const Placement byPointer = place(r);
    CHECK((byPointer.basis == PlacementBasis::Pointer));
    CHECK(byPointer.display == 1);
}

TEST_CASE("placement: CK3's window across two displays goes by the larger part") {
    PlacementRequest r = request({display({0, 0, 1920, 1080}), display({1920, 0, 1920, 1080})});
    // 420 points on the first display, 580 on the second.
    r.gameWindow = ScreenRect{1500, 100, 1000, 800};
    const Placement placed = place(r);
    CHECK(placed.display == 1);
    // On the part of the window that is on that display.
    CHECK(placed.frame == ScreenRect{1956, 160, 520, 680});
    // Mostly on the first display: the first one.
    r.gameWindow = ScreenRect{1000, 100, 1000, 800};
    CHECK(place(r).display == 0);
}

TEST_CASE("placement: a CK3 window smaller than the overlay shrinks it, down to its minimum") {
    PlacementRequest r = request({display({0, 0, 1920, 1080})});
    r.gameWindow = ScreenRect{200, 200, 400, 500};
    Placement placed = place(r);
    // 400 - 2 x 24 = 352 wide, 500 - 48 = 452 tall.
    CHECK(placed.frame == ScreenRect{224, 224, 352, 452});
    // Smaller than the minimum: the minimum, still inside the display.
    r.gameWindow = ScreenRect{200, 200, 300, 300};
    placed = place(r);
    CHECK(placed.frame.width == 340);
    CHECK(placed.frame.height == 420);
    CHECK(placed.frame == ScreenRect{136, 140, 340, 420});
    // A tiny window in the top-left corner: kept on the display.
    r.gameWindow = ScreenRect{0, 0, 200, 200};
    placed = place(r);
    CHECK(placed.frame.x == 0);
    CHECK(placed.frame.y == 0);
}

TEST_CASE("placement: a preferred size below the minimum is kept (--window-size 200x200)") {
    PlacementRequest r = request({display({0, 0, 1920, 1080})});
    r.width = 200;
    r.height = 200;
    r.pointer = ScreenPoint{10, 10};
    Placement placed = place(r);
    CHECK(placed.frame == ScreenRect{1696, 440, 200, 200});
    // On a CK3 window smaller than that it still shrinks no further than the requested size.
    r.gameWindow = ScreenRect{0, 0, 100, 100};
    placed = place(r);
    CHECK(placed.frame.width == 200);
    CHECK(placed.frame.height == 200);
}

TEST_CASE("placement: never larger than the display's visible area") {
    PlacementRequest r = request({display({0, 0, 800, 600}, {0, 25, 800, 575})});
    r.minHeight = 700;
    r.pointer = ScreenPoint{10, 10};
    const Placement placed = place(r);
    CHECK(placed.frame.height == 575);
    CHECK(placed.frame.y == 25);
    CHECK(placed.frame.width == 520);
}

TEST_CASE("placement: without a CK3 window, the pointer's display; else the primary one") {
    PlacementRequest r = request({display({0, 0, 1920, 1080}), display({1920, 0, 1920, 1080})});
    r.pointer = ScreenPoint{2500, 500};
    Placement placed = place(r);
    CHECK((placed.basis == PlacementBasis::Pointer));
    CHECK(placed.display == 1);
    CHECK(placed.frame == ScreenRect{3296, 200, 520, 680});
    // The pointer on no display (a display was just unplugged): the primary one.
    r.pointer = ScreenPoint{-5000, -5000};
    placed = place(r);
    CHECK((placed.basis == PlacementBasis::Primary));
    CHECK(placed.display == 0);
    // No pointer either.
    r.pointer.reset();
    CHECK((place(r).basis == PlacementBasis::Primary));
    // A CK3 window on no display counts as none.
    r.gameWindow = ScreenRect{10000, 10000, 800, 600};
    r.pointer = ScreenPoint{2500, 500};
    placed = place(r);
    CHECK((placed.basis == PlacementBasis::Pointer));
    CHECK(placed.display == 1);
    // An empty rectangle too.
    r.gameWindow = ScreenRect{0, 0, 0, 0};
    CHECK((place(r).basis == PlacementBasis::Pointer));
}

TEST_CASE("placement: a display without a visible area uses its frame; no display, no placement") {
    const PlacementRequest r = request({display({0, 0, 1000, 800})});
    const Placement placed = place(r);
    CHECK(placed.frame == ScreenRect{456, 60, 520, 680});
    CHECK_FALSE(curia::platform::placeOverlay(request({})).has_value());
}

TEST_CASE("placement: the bases have short names for the app log") {
    CHECK(std::string_view(toString(PlacementBasis::GameWindow)) == "game");
    CHECK(std::string_view(toString(PlacementBasis::Pointer)) == "pointer");
    CHECK(std::string_view(toString(PlacementBasis::Primary)) == "primary");
}

TEST_CASE("coordinates: Cocoa (bottom-left origin, y up) to top-left (y down) and back") {
    using curia::platform::cocoaToTopLeft;
    using curia::platform::topLeftToCocoa;
    // The primary display is 1440x900; in Cocoa its frame is 0,0,1440,900 either way.
    const double primary = 900;
    CHECK(cocoaToTopLeft(ScreenRect{0, 0, 1440, 900}, primary) == ScreenRect{0, 0, 1440, 900});
    // A display above it: in Cocoa it starts at y = 900, at the top-left it ends at y = 0.
    CHECK(cocoaToTopLeft(ScreenRect{0, 900, 1920, 1080}, primary) ==
          ScreenRect{0, -1080, 1920, 1080});
    // A taller display left of it, bottom edges level: it reaches 180 points higher.
    CHECK(cocoaToTopLeft(ScreenRect{-1920, 0, 1920, 1080}, primary) ==
          ScreenRect{-1920, -180, 1920, 1080});
    // A display below it.
    CHECK(cocoaToTopLeft(ScreenRect{0, -768, 1024, 768}, primary) == ScreenRect{0, 900, 1024, 768});
    // The menu bar strip: a visible frame of 0,0,1440,875 in Cocoa starts 25 points down.
    CHECK(cocoaToTopLeft(ScreenRect{0, 0, 1440, 875}, primary) == ScreenRect{0, 25, 1440, 875});
    // Both ways round.
    for (const ScreenRect rect : {ScreenRect{-1920, -180, 1920, 1080}, ScreenRect{12, 34, 56, 78},
                                  ScreenRect{0, -1080, 1920, 1080}}) {
        CAPTURE(rect);
        CHECK(cocoaToTopLeft(topLeftToCocoa(rect, primary), primary) == rect);
    }
    // A point: the top-left corner of the primary display is y = 900 in Cocoa.
    CHECK(cocoaToTopLeft(ScreenPoint{100, 900}, primary) == ScreenPoint{100, 0});
    CHECK(cocoaToTopLeft(ScreenPoint{100, 1000}, primary) == ScreenPoint{100, -100});
}

TEST_CASE("rectangles: overlap, containment and area") {
    const ScreenRect a{0, 0, 100, 100};
    CHECK(intersect(a, ScreenRect{50, 50, 100, 100}) == ScreenRect{50, 50, 50, 50});
    CHECK(intersect(a, ScreenRect{100, 0, 10, 10}).empty());
    CHECK(intersect(a, ScreenRect{-10, -10, 5, 5}).area() == 0);
    CHECK(a.contains(ScreenPoint{0, 0}));
    CHECK(a.contains(ScreenPoint{99.5, 99.5}));
    CHECK_FALSE(a.contains(ScreenPoint{100, 50}));  // the right edge belongs to the next display
    CHECK(ScreenRect{0, 0, 0, 10}.empty());
    CHECK(ScreenRect{0, 0, -5, 10}.area() == 0);
}
