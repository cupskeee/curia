// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <optional>

// Whether CK3 is running, asked of the platform layer on the window's once-a-second tick
// (Platform::isGameRunning()) and passed on to AppController::onCk3Running() only when the answer
// changes (the "CK3 is not running" notice and the log-silence monitor use it). UI thread only.
namespace curia {

class GamePoll {
public:
    // Asks the OS; std::nullopt = it cannot tell (the last known answer stands).
    using Query = std::function<std::optional<bool>()>;

    // `everyTicks`: ask on every n-th tick (1 = every tick; Linux reads /proc and asks less often).
    // Values below 1 count as 1.
    GamePoll(Query query, int everyTicks);

    // The first call asks at once (the poll at start-up); later calls ask on every n-th tick after
    // it. Returns the answer when it differs from the last one passed on (the first answer always
    // does), else std::nullopt.
    std::optional<bool> onTick();

    // How many times the OS was asked (the tests count).
    [[nodiscard]] int queries() const { return queries_; }
    // The last answer passed on, if any.
    [[nodiscard]] std::optional<bool> last() const { return last_; }

private:
    Query query_;
    int everyTicks_;
    int ticksUntilQuery_ = 0;
    int queries_ = 0;
    std::optional<bool> last_;
};

}  // namespace curia
