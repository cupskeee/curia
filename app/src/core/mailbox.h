// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace curia {

// A thread-safe multi-producer, single-consumer queue between the threads that produce events (the
// log watcher, the network thread, the hot key) and the UI thread, which drains it.
//
// Waking: the consumer is woken (the injected `wake` callback, which in the app pushes one SDL
// event) only when a message is queued into an EMPTY queue. While messages are waiting, further
// pushes are silent: the consumer will drain them all in one go. A stream of a thousand tokens
// therefore costs one wake-up per drain, not a thousand, and cannot flood the SDL event queue.
// `wake` runs on the producer's thread, outside the lock; it must be thread-safe and must not
// throw.
//
// Bounding, and what is dropped (decided and documented here, because it matters for answers):
//   * Nothing is ever dropped for being slow. A producer can give a `merge` function: a new
//     message that merge() folds into the newest queued message does not occupy a slot. The app
//     uses it to join consecutive text tokens of one request into one message, so a stalled UI
//     thread holds one growing string per request, not one message per token. Messages are never
//     reordered by merging (only the newest queued message can absorb the new one).
//   * A hard limit, `maxMessages`, protects against a runaway producer: a message that arrives
//     when the queue is full is dropped and counted (dropped()), unless the producer marks it
//     `force`. Terminal messages (the end of a request) are forced, so a request can never be left
//     "in flight" by the limit. With merging the limit is not reachable by token traffic.
//
// drain() returns everything queued, oldest first, and leaves the queue empty.
template <typename T> class Mailbox {
public:
    using Wake = std::function<void()>;
    // Folds `incoming` into `pending` (the newest queued message) and returns true, or returns
    // false and changes nothing. Called under the queue's lock: keep it short, no I/O.
    using Merge = std::function<bool(T& pending, T& incoming)>;

    static constexpr std::size_t kDefaultMaxMessages = 4096;

    struct Options {
        std::size_t maxMessages = kDefaultMaxMessages;
        Wake wake;
        Merge merge;
    };

    enum class PushResult { Queued, Merged, Dropped };

    Mailbox() : Mailbox(Options{}) {}
    explicit Mailbox(Options options)
        : maxMessages_(options.maxMessages), wake_(std::move(options.wake)),
          merge_(std::move(options.merge)) {}

    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;

    PushResult push(T message, bool force = false) {
        bool wasEmpty = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (merge_ && !queue_.empty() && merge_(queue_.back(), message)) {
                return PushResult::Merged;
            }
            if (!force && queue_.size() >= maxMessages_) {
                ++dropped_;
                return PushResult::Dropped;
            }
            wasEmpty = queue_.empty();
            queue_.push_back(std::move(message));
            if (queue_.size() > highWater_) {
                highWater_ = queue_.size();
            }
        }
        if (wasEmpty && wake_) {
            wake_();
        }
        return PushResult::Queued;
    }

    std::vector<T> drain() {
        std::vector<T> out;
        const std::lock_guard<std::mutex> lock(mutex_);
        out.swap(queue_);
        return out;
    }

    bool empty() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    std::size_t size() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // Messages dropped by the hard limit since creation.
    std::uint64_t dropped() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    // The most messages ever queued at once.
    std::size_t highWater() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return highWater_;
    }

private:
    const std::size_t maxMessages_;
    const Wake wake_;
    const Merge merge_;
    mutable std::mutex mutex_;
    std::vector<T> queue_;
    std::uint64_t dropped_ = 0;
    std::size_t highWater_ = 0;
};

}  // namespace curia
