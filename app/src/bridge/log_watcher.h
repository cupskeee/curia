// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "bridge/events.h"
#include "bridge/log_tailer.h"

namespace curia::bridge {

// Runs a LogTailer on its own thread. The thread sleeps on a condition variable between polls,
// so it costs nothing while the log is quiet and stop() returns at once instead of waiting out
// the interval.
//
// Polling, not OS file-change notification, on purpose: the notification APIs are deferred until
// they are tested on each OS (assumption A15, architecture.md section 5), and one stat() of a
// small file every 250 ms is far below anything measurable. Only the tailer's poll() would change.
//
// All callbacks run on the watcher thread and must not throw. The M4 UI turns them into SDL events
// (SDL_PushEvent is thread-safe) and does no work in them.
class LogWatcher {
public:
    struct Callbacks {
        // Every event, in order.
        std::function<void(Event&&)> onEvent;
        // State changes of the file (attached, truncated, missing, unreadable, recovered).
        std::function<void(TailerNotice)> onNotice;
        // After each poll that read bytes or changed state (not after an idle poll).
        std::function<void(const TailerStats&)> onProgress;
    };

    static constexpr std::chrono::milliseconds kDefaultInterval{250};

    LogWatcher(std::unique_ptr<LogTailer> tailer, Callbacks callbacks,
               std::chrono::milliseconds interval = kDefaultInterval);
    ~LogWatcher();

    LogWatcher(const LogWatcher&) = delete;
    LogWatcher& operator=(const LogWatcher&) = delete;

    // Starts the thread; false (and nothing happens) if it already runs. May be called again after
    // stop(): the tailer keeps its position, so nothing is replayed.
    bool start();
    // Stops and joins the thread; a no-op if it does not run. Must not be called from a callback.
    void stop();
    bool running() const;

    // Latest statistics, safe from any thread (refreshed after every poll that did something).
    TailerStats stats() const;

private:
    void run();

    std::unique_ptr<LogTailer> tailer_;
    Callbacks callbacks_;
    std::chrono::milliseconds interval_;

    // Serialises start() and stop(); held while joining.
    std::mutex control_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // Guards the stop flag and the statistics; the wait of the thread is on wake_.
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopRequested_ = false;
    TailerStats stats_;
};

}  // namespace curia::bridge
