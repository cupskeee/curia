// SPDX-License-Identifier: MIT
#include "bridge/log_watcher.h"

#include <utility>

namespace curia::bridge {

LogWatcher::LogWatcher(std::unique_ptr<LogTailer> tailer, Callbacks callbacks,
                       std::chrono::milliseconds interval)
    : tailer_(std::move(tailer)), callbacks_(std::move(callbacks)),
      interval_(interval.count() > 0 ? interval : std::chrono::milliseconds(1)) {}

LogWatcher::~LogWatcher() {
    stop();
}

bool LogWatcher::start() {
    const std::lock_guard<std::mutex> control(control_);
    if (thread_.joinable()) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = false;
    }
    thread_ = std::thread([this] { run(); });
    running_ = true;
    return true;
}

void LogWatcher::stop() {
    const std::lock_guard<std::mutex> control(control_);
    if (!thread_.joinable()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
    }
    wake_.notify_all();
    thread_.join();
    running_ = false;
}

bool LogWatcher::running() const {
    return running_;
}

TailerStats LogWatcher::stats() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void LogWatcher::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopRequested_) {
        lock.unlock();
        PollResult result = tailer_->poll();
        const bool active =
            result.bytesRead > 0 || !result.notices.empty() || !result.events.empty();
        const TailerStats stats = tailer_->stats();
        if (active) {
            {
                const std::lock_guard<std::mutex> statsLock(mutex_);
                stats_ = stats;
            }
            if (callbacks_.onNotice) {
                for (const TailerNotice notice : result.notices) {
                    callbacks_.onNotice(notice);
                }
            }
            if (callbacks_.onEvent) {
                for (Event& event : result.events) {
                    callbacks_.onEvent(std::move(event));
                }
            }
            if (callbacks_.onProgress) {
                callbacks_.onProgress(stats);
            }
        }
        lock.lock();
        // Returns early when stop() sets the flag and notifies; the predicate also covers a stop
        // that arrived while the poll above was running.
        wake_.wait_for(lock, interval_, [this] { return stopRequested_; });
    }
}

}  // namespace curia::bridge
