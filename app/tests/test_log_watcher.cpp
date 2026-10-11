// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

#include "bridge/log_watcher.h"
#include "bridge_test_support.h"

using namespace curia::bridge;
using namespace curia::bridge::test;
using namespace std::chrono_literals;

namespace {

// Collects what the watcher delivers; the test thread waits on it.
class Collector {
public:
    LogWatcher::Callbacks callbacks() {
        LogWatcher::Callbacks cb;
        cb.onEvent = [this](Event&& event) {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (std::holds_alternative<FrameComplete>(event)) {
                ++frames_;
            }
            threads_.push_back(std::this_thread::get_id());
            changed_.notify_all();
        };
        cb.onNotice = [this](TailerNotice notice) {
            const std::lock_guard<std::mutex> lock(mutex_);
            notices_.push_back(notice);
            changed_.notify_all();
        };
        return cb;
    }

    bool waitForFrames(int count, std::chrono::milliseconds limit = 5s) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, limit, [&] { return frames_ >= count; });
    }

    bool waitForNotices(std::size_t count, std::chrono::milliseconds limit = 5s) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, limit, [&] { return notices_.size() >= count; });
    }

    int frames() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }
    std::vector<std::thread::id> threads() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return threads_;
    }
    std::vector<TailerNotice> notices() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return notices_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int frames_ = 0;
    std::vector<std::thread::id> threads_;
    std::vector<TailerNotice> notices_;
};

std::unique_ptr<LogTailer> tailerOn(const std::shared_ptr<FakeFile>& file) {
    return std::make_unique<LogTailer>(std::make_unique<FakeFileSource>(file));
}

}  // namespace

TEST_CASE("watcher: delivers events on its own thread") {
    auto file = std::make_shared<FakeFile>();
    Collector collector;
    LogWatcher watcher(tailerOn(file), collector.callbacks(), 5ms);
    CHECK_FALSE(watcher.running());
    REQUIRE(watcher.start());
    CHECK(watcher.running());
    // The watcher attaches at the end of what exists; bytes written before its first poll would be
    // history, so wait for the attach.
    REQUIRE(collector.waitForNotices(1));

    file->append(stream(exampleFrame(1)));
    REQUIRE(collector.waitForFrames(1));
    watcher.stop();
    CHECK_FALSE(watcher.running());

    const std::vector<std::thread::id> threads = collector.threads();
    REQUIRE_FALSE(threads.empty());
    CHECK(threads.front() != std::this_thread::get_id());
    CHECK(watcher.stats().totalBytes == file->size());
    CHECK((collector.notices().front() == TailerNotice::Attached));
}

TEST_CASE("watcher: stop returns at once, not after the poll interval") {
    auto file = std::make_shared<FakeFile>();
    Collector collector;
    LogWatcher watcher(tailerOn(file), collector.callbacks(), 60s);
    REQUIRE(watcher.start());
    std::this_thread::sleep_for(50ms);  // let it reach its wait
    const auto begin = std::chrono::steady_clock::now();
    watcher.stop();
    const auto took = std::chrono::steady_clock::now() - begin;
    CHECK(took < 5s);  // generous for a loaded CI machine; the interval is a minute
}

TEST_CASE("watcher: start and stop can be repeated and nothing is replayed after a restart") {
    auto file = std::make_shared<FakeFile>();
    Collector collector;
    LogWatcher watcher(tailerOn(file), collector.callbacks(), 5ms);
    watcher.stop();  // not running: a no-op
    REQUIRE(watcher.start());
    CHECK_FALSE(watcher.start());  // already running
    REQUIRE(collector.waitForNotices(1));

    file->append(stream(exampleFrame(1)));
    REQUIRE(collector.waitForFrames(1));
    watcher.stop();
    watcher.stop();

    // Written while stopped: delivered after the restart, and the first frame is not delivered
    // twice.
    file->append(stream(exampleFrame(2)));
    REQUIRE(watcher.start());
    REQUIRE(collector.waitForFrames(2));
    watcher.stop();
    CHECK(collector.frames() == 2);
}

TEST_CASE("watcher: polls at the interval and does not spin") {
    auto file = std::make_shared<FakeFile>();
    Collector collector;
    LogWatcher watcher(tailerOn(file), collector.callbacks(), 50ms);
    REQUIRE(watcher.start());
    std::this_thread::sleep_for(400ms);
    watcher.stop();
    // About 8 polls expected; a busy loop would make thousands. The bounds are loose on purpose.
    CHECK(file->statCalls() >= 2);
    CHECK(file->statCalls() <= 40);
}

TEST_CASE("watcher: the destructor stops a running watcher") {
    auto file = std::make_shared<FakeFile>();
    Collector collector;
    {
        LogWatcher watcher(tailerOn(file), collector.callbacks(), 60s);
        REQUIRE(watcher.start());
    }
    CHECK(true);  // reaching here means the destructor joined the thread
}

TEST_CASE("watcher: notices and progress are delivered") {
    auto file = std::make_shared<FakeFile>();
    file->remove();
    Collector collector;
    std::mutex mutex;
    int progress = 0;
    LogWatcher::Callbacks callbacks = collector.callbacks();
    callbacks.onProgress = [&](const TailerStats&) {
        const std::lock_guard<std::mutex> lock(mutex);
        ++progress;
    };
    LogWatcher watcher(tailerOn(file), std::move(callbacks), 5ms);
    REQUIRE(watcher.start());
    REQUIRE(collector.waitForNotices(1));  // Missing, before the file exists
    file->create();
    file->append(stream(exampleFrame(3)));
    REQUIRE(collector.waitForFrames(1));
    watcher.stop();
    const std::vector<TailerNotice> notices = collector.notices();
    REQUIRE(notices.size() >= 2);
    CHECK((notices.front() == TailerNotice::Missing));
    CHECK((notices.back() == TailerNotice::Attached));
    const std::lock_guard<std::mutex> lock(mutex);
    CHECK(progress >= 1);
}
