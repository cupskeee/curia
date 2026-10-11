// SPDX-License-Identifier: MIT
#include "core/watch_log.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <variant>

#include "bridge/frame_json.h"
#include "bridge/log_tailer.h"
#include "bridge/log_watcher.h"

namespace curia {

int runWatchLog(const CliOptions& options, std::ostream& out, std::ostream& err) {
    using namespace curia::bridge;

    std::mutex mutex;
    std::condition_variable doneChanged;
    bool done = false;
    int frames = 0;

    LogWatcher::Callbacks callbacks;
    callbacks.onEvent = [&](Event&& event) {
        const std::lock_guard<std::mutex> lock(mutex);
        if (done) {
            return;  // nothing after the asked-for last frame
        }
        out << dumpJsonLine(toJson(event)) << '\n' << std::flush;
        if (std::holds_alternative<FrameComplete>(event)) {
            ++frames;
            if (options.maxFrames > 0 && frames >= options.maxFrames) {
                done = true;
                doneChanged.notify_all();
            }
        }
    };
    callbacks.onNotice = [&](TailerNotice notice) {
        err << "curia: log " << toString(notice) << '\n' << std::flush;
    };
    callbacks.onProgress = [&](const TailerStats& stats) {
        err << "curia: offset " << stats.offset << " total " << stats.totalBytes << " curia "
            << stats.curiaBytes << '\n'
            << std::flush;
    };

    LogTailer::Options tailerOptions;
    tailerOptions.fromStart = options.fromStart;
    auto tailer = std::make_unique<LogTailer>(std::make_unique<FileSource>(options.watchLogPath),
                                              tailerOptions);
    LogWatcher watcher(std::move(tailer), std::move(callbacks));
    watcher.start();

    const int timeoutSeconds =
        options.timeoutSeconds > 0 ? options.timeoutSeconds : kDefaultWatchTimeoutSeconds;
    bool reached = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        reached =
            doneChanged.wait_for(lock, std::chrono::seconds(timeoutSeconds), [&] { return done; });
    }
    watcher.stop();

    if (reached || options.maxFrames == 0) {
        return 0;
    }
    err << "curia: timeout after " << timeoutSeconds << " s with " << frames << " of "
        << options.maxFrames << " complete frames\n"
        << std::flush;
    return 3;
}

}  // namespace curia
