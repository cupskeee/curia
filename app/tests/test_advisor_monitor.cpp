// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <optional>

#include "advisor/log_silence_monitor.h"
#include "advisor_test_support.h"
#include "core/ui_pacing.h"

using namespace curia;
using namespace curia::advisor;
using namespace curia::advisor::test;
using std::chrono::seconds;

namespace {

// The defaults of the plan, in a form that is easy to read: arm at 120 s, wait 15 s, passive 600 s.
struct Rig {
    explicit Rig(LogSilenceConfig config = {}) : monitor(clock.fn(), config) {}

    void advance(long long secs) {
        clock.advance(seconds(secs));
        monitor.tick(clock.now);
    }

    FakeClock clock;
    LogSilenceMonitor monitor;
};

}  // namespace

TEST_CASE("monitor: starts quiet") {
    Rig rig;
    CHECK((rig.monitor.notice() == Notice::None));
    rig.advance(100000);
    CHECK((rig.monitor.notice() == Notice::None));
    CHECK_FALSE(rig.monitor.secondsSinceLogGrew(rig.clock.now).has_value());
}

TEST_CASE("monitor armed: opened with no snapshot, no frame in 15 s, restart CK3") {
    Rig rig;
    rig.monitor.overlayOpenedByUser();
    CHECK((rig.monitor.notice() == Notice::None));
    rig.advance(14);
    CHECK((rig.monitor.notice() == Notice::None));
    rig.advance(1);
    CHECK((rig.monitor.notice() == Notice::RestartCk3));
    rig.advance(3600);
    CHECK((rig.monitor.notice() == Notice::RestartCk3));  // stays until a frame arrives
    rig.monitor.frameArrived();
    CHECK((rig.monitor.notice() == Notice::None));
}

TEST_CASE("monitor armed: a snapshot younger than two minutes is fresh enough") {
    Rig rig;
    rig.monitor.frameArrived();
    rig.advance(119);
    rig.monitor.overlayOpenedByUser();
    rig.advance(60);
    CHECK((rig.monitor.notice() == Notice::None));
}

TEST_CASE("monitor armed: a snapshot of two minutes or more arms it") {
    Rig rig;
    rig.monitor.frameArrived();
    rig.advance(120);
    rig.monitor.overlayOpenedByUser();
    rig.advance(14);
    CHECK((rig.monitor.notice() == Notice::None));
    rig.advance(1);
    CHECK((rig.monitor.notice() == Notice::RestartCk3));
}

TEST_CASE("monitor armed: a frame in the window disarms") {
    Rig rig;
    rig.monitor.overlayOpenedByUser();
    rig.advance(10);
    rig.monitor.frameArrived();
    rig.advance(60);
    CHECK((rig.monitor.notice() == Notice::None));
}

TEST_CASE("monitor armed: a damaged frame in the window disarms, it is a different problem") {
    Rig rig;
    rig.monitor.overlayOpenedByUser();
    rig.advance(10);
    rig.monitor.frameDamaged();
    rig.advance(60);
    CHECK((rig.monitor.notice() == Notice::None));
}

TEST_CASE("monitor armed: unrelated log growth does not disarm it") {
    Rig rig;
    rig.monitor.overlayOpenedByUser();
    rig.advance(5);
    rig.monitor.logGrew();
    rig.advance(5);
    rig.monitor.logGrew();
    rig.advance(5);
    CHECK((rig.monitor.notice() == Notice::RestartCk3));
}

TEST_CASE("monitor armed: opening again does not extend the wait; after the notice it stays") {
    Rig rig;
    rig.monitor.overlayOpenedByUser();
    rig.advance(10);
    rig.monitor.overlayOpenedByUser();
    rig.advance(5);
    CHECK((rig.monitor.notice() == Notice::RestartCk3));
    rig.monitor.overlayOpenedByUser();
    rig.advance(100);
    CHECK((rig.monitor.notice() == Notice::RestartCk3));
}

TEST_CASE("monitor passive: silence of ten minutes after the log grew") {
    Rig rig;
    rig.monitor.logGrew();
    rig.advance(599);
    CHECK((rig.monitor.notice() == Notice::None));
    rig.advance(1);
    CHECK((rig.monitor.notice() == Notice::SilentSoft));
    rig.monitor.logGrew();
    CHECK((rig.monitor.notice() == Notice::None));
    rig.advance(600);
    CHECK((rig.monitor.notice() == Notice::SilentSoft));
    rig.monitor.frameArrived();
    CHECK((rig.monitor.notice() == Notice::None));
}

TEST_CASE("monitor passive: a log that never grew is not silent") {
    Rig rig;
    rig.advance(100000);
    CHECK((rig.monitor.notice() == Notice::None));
}

TEST_CASE("monitor: restart CK3 beats the soft notice") {
    Rig rig;
    rig.monitor.logGrew();
    rig.advance(1000);
    CHECK((rig.monitor.notice() == Notice::SilentSoft));
    rig.monitor.overlayOpenedByUser();
    rig.advance(15);
    CHECK((rig.monitor.notice() == Notice::RestartCk3));
}

TEST_CASE("monitor: CK3 not running beats restart CK3 and the soft notice") {
    // Armed and expired, then CK3 turns out not to run.
    Rig rig;
    rig.monitor.overlayOpenedByUser();
    rig.advance(20);
    REQUIRE((rig.monitor.notice() == Notice::RestartCk3));
    rig.monitor.ck3Running(false);
    CHECK((rig.monitor.notice() == Notice::Ck3NotRunning));
    rig.advance(5000);
    CHECK((rig.monitor.notice() == Notice::Ck3NotRunning));
    // CK3 starts again: the old judgement is gone.
    rig.monitor.ck3Running(true);
    CHECK((rig.monitor.notice() == Notice::None));
    rig.advance(5000);
    CHECK((rig.monitor.notice() == Notice::None));

    // Soft notice, then not running.
    Rig soft;
    soft.monitor.logGrew();
    soft.advance(700);
    REQUIRE((soft.monitor.notice() == Notice::SilentSoft));
    soft.monitor.ck3Running(false);
    CHECK((soft.monitor.notice() == Notice::Ck3NotRunning));
    soft.monitor.ck3Running(true);
    CHECK((soft.monitor.notice() == Notice::None));  // the time CK3 was closed is not silence
    soft.advance(599);
    CHECK((soft.monitor.notice() == Notice::None));
}

TEST_CASE("monitor: opening the overlay while CK3 is closed arms nothing") {
    Rig rig;
    rig.monitor.ck3Running(false);
    rig.monitor.overlayOpenedByUser();
    rig.advance(60);
    CHECK((rig.monitor.notice() == Notice::Ck3NotRunning));
    rig.monitor.ck3Running(true);
    CHECK((rig.monitor.notice() == Notice::None));
    rig.advance(60);
    CHECK((rig.monitor.notice() == Notice::None));
}

TEST_CASE("monitor: not knowing whether CK3 runs counts as running") {
    Rig rig;
    rig.monitor.overlayOpenedByUser();
    rig.advance(15);
    CHECK((rig.monitor.notice() == Notice::RestartCk3));
    // An explicit "running" changes nothing.
    rig.monitor.ck3Running(true);
    CHECK((rig.monitor.notice() == Notice::RestartCk3));
}

TEST_CASE("monitor: thresholds come from the constructor") {
    LogSilenceConfig config;
    config.armSnapshotAge = seconds(2);
    config.armWait = seconds(1);
    config.passiveSilence = seconds(3);
    Rig rig(config);
    rig.monitor.logGrew();
    rig.advance(2);
    CHECK((rig.monitor.notice() == Notice::None));
    rig.advance(1);
    CHECK((rig.monitor.notice() == Notice::SilentSoft));
    rig.monitor.frameArrived();
    rig.advance(2);
    rig.monitor.overlayOpenedByUser();
    rig.advance(1);
    CHECK((rig.monitor.notice() == Notice::RestartCk3));
}

TEST_CASE("monitor: seconds since the log grew") {
    Rig rig;
    rig.monitor.logGrew();
    rig.clock.advance(seconds(42));
    const std::optional<long long> now = rig.monitor.secondsSinceLogGrew(rig.clock.now);
    CHECK((now == 42));
    // A time before the growth gives 0, not a negative number.
    CHECK((rig.monitor.secondsSinceLogGrew(rig.clock.now - seconds(100)) == 0));
}

TEST_CASE("notice names") {
    CHECK(std::string(toString(Notice::None)) == "None");
    CHECK(std::string(toString(Notice::RestartCk3)) == "RestartCk3");
    CHECK(std::string(toString(Notice::SilentSoft)) == "SilentSoft");
    CHECK(std::string(toString(Notice::Ck3NotRunning)) == "Ck3NotRunning");
}

// --- ui pacing --------------------------------------------------------------------------------

TEST_CASE("ui pacing: the full table") {
    struct Row {
        bool text;
        bool spinner;
        bool hidden;
        bool pending;
        WaitPolicy::Kind kind;
        int ms;
    };
    constexpr auto B = WaitPolicy::Kind::Block;
    constexpr auto T = WaitPolicy::Kind::Timeout;
    const std::array<Row, 12> rows = {{
        // text spinner hidden pending
        {false, false, false, false, B, 0},
        {true, false, false, false, T, kCaretBlinkMs},
        {false, true, false, false, T, kSpinnerFrameMs},
        {true, true, false, false, T, kSpinnerFrameMs},
        // A hidden overlay stays blocked, whatever else is going on.
        {false, false, true, false, B, 0},
        {true, false, true, false, B, 0},
        {false, true, true, false, B, 0},
        {true, true, true, false, B, 0},
        // Pending messages: do not wait.
        {false, false, false, true, T, 0},
        {true, true, false, true, T, 0},
        {false, false, true, true, T, 0},
        {true, true, true, true, T, 0},
    }};
    for (const Row& row : rows) {
        CAPTURE(row.text);
        CAPTURE(row.spinner);
        CAPTURE(row.hidden);
        CAPTURE(row.pending);
        const WaitPolicy policy = uiWaitPolicy(row.text, row.spinner, row.hidden, row.pending);
        CHECK((policy.kind == row.kind));
        CHECK(policy.timeoutMs == row.ms);
    }
    CHECK(kSpinnerFrameMs < kCaretBlinkMs);
}
