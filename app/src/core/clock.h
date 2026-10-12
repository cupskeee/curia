// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <functional>

namespace curia {

// The time source of every piece of logic that measures real time (snapshot age, log silence,
// latencies). It is monotonic on purpose: a change of the wall clock must not make a snapshot look
// older or younger. Production code passes steadyNow; tests pass a fake that they advance by hand,
// so no test sleeps and none depends on the machine's speed.
using TimePoint = std::chrono::steady_clock::time_point;
using ClockFn = std::function<TimePoint()>;

inline TimePoint steadyNow() {
    return std::chrono::steady_clock::now();
}

// Whole seconds from `from` to `to`, never negative.
inline long long secondsBetween(TimePoint from, TimePoint to) {
    if (to <= from) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(to - from).count();
}

}  // namespace curia
