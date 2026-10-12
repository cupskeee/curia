// SPDX-License-Identifier: MIT
#include "advisor/log_silence_monitor.h"

#include <utility>

namespace curia::advisor {

const char* toString(Notice notice) noexcept {
    switch (notice) {
    case Notice::None:
        return "None";
    case Notice::RestartCk3:
        return "RestartCk3";
    case Notice::SilentSoft:
        return "SilentSoft";
    case Notice::Ck3NotRunning:
        return "Ck3NotRunning";
    }
    return "Unknown";
}

LogSilenceMonitor::LogSilenceMonitor(ClockFn clock, LogSilenceConfig config)
    : clock_(std::move(clock)), config_(config) {}

void LogSilenceMonitor::logGrew() {
    const TimePoint now = clock_();
    lastGrowth_ = now;
    evaluate(now);
}

void LogSilenceMonitor::frameArrived() {
    const TimePoint now = clock_();
    lastGrowth_ = now;
    lastFrame_ = now;
    armedAt_.reset();
    restartNeeded_ = false;
    evaluate(now);
}

void LogSilenceMonitor::frameDamaged() {
    const TimePoint now = clock_();
    lastGrowth_ = now;
    armedAt_.reset();
    restartNeeded_ = false;
    evaluate(now);
}

void LogSilenceMonitor::overlayOpenedByUser() {
    const TimePoint now = clock_();
    const bool oldSnapshot =
        !lastFrame_.has_value() || (now - *lastFrame_) >= config_.armSnapshotAge;
    if (oldSnapshot && running_.value_or(true) && !armedAt_.has_value() && !restartNeeded_) {
        armedAt_ = now;
    }
    evaluate(now);
}

void LogSilenceMonitor::ck3Running(bool running) {
    const bool wasStopped = running_.has_value() && !*running_;
    running_ = running;
    if (!running || wasStopped) {
        // A closed game ends every pending judgement about its log: the time it spent closed is
        // not silence, and a restart is what "restart CK3" asked for.
        armedAt_.reset();
        restartNeeded_ = false;
        lastGrowth_.reset();
    }
    evaluate(clock_());
}

void LogSilenceMonitor::tick(TimePoint now) {
    evaluate(now);
}

std::optional<long long> LogSilenceMonitor::secondsSinceLogGrew(TimePoint now) const {
    if (!lastGrowth_.has_value()) {
        return std::nullopt;
    }
    return secondsBetween(*lastGrowth_, now);
}

void LogSilenceMonitor::evaluate(TimePoint now) {
    const bool running = running_.value_or(true);
    if (!running) {
        notice_ = Notice::Ck3NotRunning;
        return;
    }
    if (armedAt_.has_value() && (now - *armedAt_) >= config_.armWait) {
        armedAt_.reset();
        restartNeeded_ = true;
    }
    if (restartNeeded_) {
        notice_ = Notice::RestartCk3;
        return;
    }
    if (lastGrowth_.has_value() && (now - *lastGrowth_) >= config_.passiveSilence) {
        notice_ = Notice::SilentSoft;
        return;
    }
    notice_ = Notice::None;
}

}  // namespace curia::advisor
