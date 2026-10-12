// SPDX-License-Identifier: MIT
#include "advisor/snapshot_store.h"

#include <tuple>
#include <utility>

#include "advisor/snapshot_renderer.h"

namespace curia::advisor {

namespace {

std::tuple<int, int, int> dateKey(const bridge::Date& date) {
    return {date.year, date.month, date.day};
}

}  // namespace

SnapshotStore::SnapshotStore(ClockFn clock) : clock_(std::move(clock)), text_(renderNoSnapshot()) {}

SnapshotStore::Update SnapshotStore::onFrameComplete(bridge::Frame frame) {
    Update update;
    update.first = !frame_.has_value();
    if (frame_.has_value()) {
        update.dateWentBack = dateKey(frame.date) < dateKey(frame_->date);
    }
    text_ = renderSnapshot(frame);
    frame_ = std::move(frame);
    arrivedAt_ = clock_();
    unreadable_ = false;
    ++sequence_;
    return update;
}

void SnapshotStore::onExportUnreadable() {
    unreadable_ = true;
}

Freshness SnapshotStore::freshness(TimePoint now, long long logSilentSeconds) const {
    Freshness result;
    result.logSilentSeconds = logSilentSeconds < 0 ? 0 : logSilentSeconds;
    result.exportUnreadable = unreadable_;
    if (arrivedAt_.has_value()) {
        result.snapshotAgeSeconds = secondsBetween(*arrivedAt_, now);
    }
    if (frame_.has_value()) {
        result.gameDateOfSnapshot = frame_->date;
    }
    return result;
}

}  // namespace curia::advisor
