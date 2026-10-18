// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <optional>
#include <vector>

#include "core/game_poll.h"
#include "platform/game_window.h"
#include "platform/platform.h"

using curia::platform::isGameProcessName;
using curia::platform::pickGameWindow;
using curia::platform::ScreenRect;
using curia::platform::WindowCandidate;

namespace {

WindowCandidate window(long long pid, ScreenRect rect, int layer = 0) {
    WindowCandidate candidate;
    candidate.pid = pid;
    candidate.layer = layer;
    candidate.rect = rect;
    return candidate;
}

bool same(const std::optional<ScreenRect>& found, const ScreenRect& expected) {
    return found.has_value() && found.value_or(ScreenRect{}) == expected;
}

}  // namespace

TEST_CASE("game process: ck3 (macOS, native Linux) and ck3.exe (Windows, Proton), any case") {
    for (const char* name : {"ck3", "CK3", "Ck3", "ck3.exe", "CK3.EXE", "ck3.Exe"}) {
        CAPTURE(name);
        CHECK(isGameProcessName(name));
    }
    for (const char* name : {"", "ck", "ck3x", "ck3.ex", "ck3.exe ", " ck3", "ck3.app", "steam",
                             "dowser", "ck2", "ck3.exe.bak"}) {
        CAPTURE(name);
        CHECK_FALSE(isGameProcessName(name));
    }
}

TEST_CASE("game window: the largest layer-0 window of the game, as on the owner's Mac (M2)") {
    // CK3's large window beside thin strips on the other display, a menu-level window and windows
    // of other processes (M2, E9).
    const std::vector<WindowCandidate> windows = {
        window(77, {0, 0, 2056, 39}, 25),      // a status-level strip of CK3
        window(12, {0, 0, 3000, 2000}),        // another app's window, larger
        window(77, {2056, 0, 1920, 4}),        // a thin strip on the other display
        window(77, {0, 39, 2056, 1290}),       // the game
        window(77, {100, 100, 0, 300}),        // empty
        window(77, {0, 0, 5000, 5000}, 1000),  // a high-level window
        window(88, {0, 0, 4000, 3000}),        // another process (a second CK3 below)
    };
    CHECK(same(pickGameWindow(windows, {77}), ScreenRect{0, 39, 2056, 1290}));
    // Two CK3 processes (a restart in progress): the largest window of either.
    CHECK(same(pickGameWindow(windows, {77, 88}), ScreenRect{0, 0, 4000, 3000}));
}

TEST_CASE("game window: the larger area wins whatever the order, ties keep the front-most") {
    std::vector<WindowCandidate> windows = {window(5, {10, 10, 300, 200}),
                                            window(5, {0, 0, 800, 600})};
    CHECK(same(pickGameWindow(windows, {5}), ScreenRect{0, 0, 800, 600}));
    windows = {window(5, {0, 0, 800, 600}), window(5, {10, 10, 300, 200})};
    CHECK(same(pickGameWindow(windows, {5}), ScreenRect{0, 0, 800, 600}));
    windows = {window(5, {0, 0, 800, 600}), window(5, {50, 50, 800, 600})};
    CHECK(same(pickGameWindow(windows, {5}), ScreenRect{0, 0, 800, 600}));
}

TEST_CASE("game window: nothing found") {
    CHECK_FALSE(pickGameWindow({}, {5}).has_value());
    CHECK_FALSE(pickGameWindow({window(5, {0, 0, 800, 600})}, {}).has_value());
    CHECK_FALSE(pickGameWindow({window(6, {0, 0, 800, 600})}, {5}).has_value());
    CHECK_FALSE(pickGameWindow({window(5, {0, 0, 800, 600}, 3)}, {5}).has_value());
    CHECK_FALSE(pickGameWindow({window(5, {0, 0, 800, 0})}, {5}).has_value());
}

TEST_CASE("the platform layer answers the game queries without a window") {
    const auto platform = curia::platform::create();
    REQUIRE(platform != nullptr);
    // Whatever runs on this machine: an answer (or "cannot tell"), never a crash or a prompt.
    const std::optional<bool> running = platform->isGameRunning();
    const std::optional<bool> front = platform->gameIsFrontmost();
    if (running.has_value() && !running.value_or(true)) {
        // No CK3, so it cannot be in front and has no window.
        CHECK_FALSE(front.value_or(false));
        CHECK_FALSE(platform->findGameWindow().has_value());
    }
    const std::optional<ScreenRect> found = platform->findGameWindow();
    if (found.has_value()) {
        CHECK_FALSE(found.value_or(ScreenRect{}).empty());
    }
    CHECK(platform->gameRunningPollTicks() >= 1);
#if defined(__APPLE__) || defined(_WIN32)
    CHECK(running.has_value());
    CHECK(front.has_value());
#endif
}

TEST_CASE("game poll: asks at once, then every n-th tick, and passes on changes only") {
    std::optional<bool> answer = false;
    curia::GamePoll poll([&answer] { return answer; }, 3);
    // The first tick (start-up) asks and passes on the first answer.
    CHECK(poll.onTick() == std::optional<bool>(false));
    CHECK(poll.queries() == 1);
    // Ticks 2 and 3 do not ask; tick 4 asks again.
    answer = true;
    CHECK_FALSE(poll.onTick().has_value());
    CHECK_FALSE(poll.onTick().has_value());
    CHECK(poll.queries() == 1);
    CHECK(poll.onTick() == std::optional<bool>(true));
    CHECK(poll.queries() == 2);
    // The same answer again is not passed on.
    for (int i = 0; i < 3; ++i) {
        CHECK_FALSE(poll.onTick().has_value());
    }
    CHECK(poll.queries() == 3);
    // "Cannot tell" keeps the last answer.
    answer.reset();
    for (int i = 0; i < 3; ++i) {
        CHECK_FALSE(poll.onTick().has_value());
    }
    CHECK(poll.last() == std::optional<bool>(true));
    answer = false;
    for (int i = 0; i < 2; ++i) {
        CHECK_FALSE(poll.onTick().has_value());
    }
    CHECK(poll.onTick() == std::optional<bool>(false));
}

TEST_CASE("game poll: every tick, and odd settings") {
    int asked = 0;
    curia::GamePoll everyTick(
        [&asked] {
            ++asked;
            return std::optional<bool>(asked % 2 == 0);
        },
        1);
    CHECK(everyTick.onTick() == std::optional<bool>(false));
    CHECK(everyTick.onTick() == std::optional<bool>(true));
    CHECK(everyTick.onTick() == std::optional<bool>(false));
    CHECK(asked == 3);
    // Below 1 counts as 1.
    curia::GamePoll zero([] { return std::optional<bool>(true); }, 0);
    CHECK(zero.onTick() == std::optional<bool>(true));
    CHECK_FALSE(zero.onTick().has_value());
    CHECK(zero.queries() == 2);
    // No query: nothing is asked, nothing passed on.
    curia::GamePoll none(curia::GamePoll::Query{}, 1);
    CHECK_FALSE(none.onTick().has_value());
    CHECK(none.queries() == 0);
}
