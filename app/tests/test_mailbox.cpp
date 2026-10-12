// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "core/mailbox.h"
#include "core/ui_message.h"

using namespace curia;

namespace {

UiMessage token(std::uint64_t generation, std::string text,
                providers::TokenKind kind = providers::TokenKind::Text) {
    return UiMessage{ProviderToken{generation, kind, std::move(text)}};
}

}  // namespace

TEST_CASE("mailbox: wakes only on the empty to non-empty transition") {
    int wakes = 0;
    Mailbox<int>::Options options;
    options.wake = [&wakes] { ++wakes; };
    Mailbox<int> box(options);

    CHECK(box.empty());
    box.push(1);
    CHECK(wakes == 1);
    box.push(2);
    box.push(3);
    CHECK(wakes == 1);
    CHECK(box.size() == 3);

    const std::vector<int> all = box.drain();
    CHECK((all == std::vector<int>{1, 2, 3}));
    CHECK(box.empty());
    CHECK(box.drain().empty());
    CHECK(wakes == 1);

    box.push(4);
    CHECK(wakes == 2);
    CHECK((box.drain() == std::vector<int>{4}));
}

TEST_CASE("mailbox: a mailbox without a wake callback works") {
    Mailbox<std::string> box;
    box.push("a");
    box.push("b");
    CHECK((box.drain() == std::vector<std::string>{"a", "b"}));
}

TEST_CASE("mailbox: the hard limit drops and counts, forced messages always get in") {
    Mailbox<int>::Options options;
    options.maxMessages = 3;
    Mailbox<int> box(options);
    for (int i = 0; i < 5; ++i) {
        const auto result = box.push(i);
        CHECK((result ==
               (i < 3 ? Mailbox<int>::PushResult::Queued : Mailbox<int>::PushResult::Dropped)));
    }
    CHECK(box.size() == 3);
    CHECK(box.dropped() == 2);
    CHECK((box.push(99, true) == Mailbox<int>::PushResult::Queued));
    CHECK(box.size() == 4);
    CHECK(box.highWater() == 4);
    CHECK((box.drain() == std::vector<int>{0, 1, 2, 99}));
    CHECK(box.dropped() == 2);
}

TEST_CASE("ui mailbox: consecutive tokens of one request join into one message") {
    int wakes = 0;
    const auto box = makeUiMailbox([&wakes] { ++wakes; });
    for (int i = 0; i < 1000; ++i) {
        box->push(token(1, "t"));
    }
    CHECK(box->size() == 1);
    CHECK(wakes == 1);
    const std::vector<UiMessage> messages = box->drain();
    REQUIRE(messages.size() == 1);
    CHECK((std::get<ProviderToken>(messages[0]).text == std::string(1000, 't')));
}

TEST_CASE(
    "ui mailbox: tokens of another request, another kind or after another message stay apart") {
    const auto box = makeUiMailbox({});
    box->push(token(1, "a"));
    box->push(token(2, "b"));
    box->push(token(2, "c", providers::TokenKind::Thinking));
    box->push(UiMessage{ProviderStatus{2, providers::StreamStatus::TextStarted}});
    box->push(token(2, "d"));
    box->push(token(2, "e"));
    const std::vector<UiMessage> messages = box->drain();
    REQUIRE(messages.size() == 5);
    CHECK((std::get<ProviderToken>(messages[0]).text == "a"));
    CHECK((std::get<ProviderToken>(messages[1]).text == "b"));
    CHECK((std::get<ProviderToken>(messages[2]).kind == providers::TokenKind::Thinking));
    CHECK(std::holds_alternative<ProviderStatus>(messages[3]));
    CHECK((std::get<ProviderToken>(messages[4]).text == "de"));
}

TEST_CASE("ui mailbox: merging never reorders and is capped in size") {
    const auto box = makeUiMailbox({});
    const std::string chunk(kMaxCoalescedTokenBytes / 2, 'x');
    box->push(token(1, chunk));
    box->push(token(1, chunk));
    box->push(token(1, "more"));
    const std::vector<UiMessage> messages = box->drain();
    REQUIRE(messages.size() == 2);
    CHECK(std::get<ProviderToken>(messages[0]).text.size() == kMaxCoalescedTokenBytes);
    CHECK((std::get<ProviderToken>(messages[1]).text == "more"));
}

TEST_CASE("ui mailbox: terminal messages are not lost to the limit") {
    const auto box = makeUiMailbox({}, 2);
    box->push(UiMessage{LogNotice{bridge::TailerNotice::Attached}});
    box->push(UiMessage{LogNotice{bridge::TailerNotice::Truncated}});
    box->push(UiMessage{LogNotice{bridge::TailerNotice::Missing}});  // dropped
    box->push(UiMessage{ProviderDone{1, {}}}, true);
    CHECK(box->dropped() == 1);
    const std::vector<UiMessage> messages = box->drain();
    REQUIRE(messages.size() == 3);
    CHECK(std::holds_alternative<ProviderDone>(messages.back()));
}

TEST_CASE("mailbox: many producers, one consumer, nothing lost, order kept per producer") {
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 4000;

    std::mutex wakeMutex;
    std::condition_variable wakeSignal;
    std::atomic<int> wakes{0};
    Mailbox<std::pair<int, int>>::Options options;
    // Large enough that the hard limit cannot be reached here (it is tested on its own above).
    options.maxMessages = static_cast<std::size_t>(kProducers) * kPerProducer;
    options.wake = [&] {
        {
            const std::lock_guard<std::mutex> lock(wakeMutex);
            ++wakes;
        }
        wakeSignal.notify_one();
    };
    Mailbox<std::pair<int, int>> box(options);

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&box, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                box.push({p, i});
            }
        });
    }

    std::vector<int> next(kProducers, 0);
    int received = 0;
    bool orderOk = true;
    int lastSeenWakes = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (received < kProducers * kPerProducer && std::chrono::steady_clock::now() < deadline) {
        {
            std::unique_lock<std::mutex> lock(wakeMutex);
            // Sleep until the producers say something arrived (or a short while: a wake-up that
            // came before this wait is covered by the check of the queue below).
            wakeSignal.wait_for(lock, std::chrono::milliseconds(50),
                                [&] { return wakes.load() != lastSeenWakes; });
            lastSeenWakes = wakes.load();
        }
        for (const auto& [producer, index] : box.drain()) {
            orderOk = orderOk && next[static_cast<std::size_t>(producer)] == index;
            next[static_cast<std::size_t>(producer)] = index + 1;
            ++received;
        }
    }
    for (std::thread& thread : producers) {
        thread.join();
    }
    CHECK(received == kProducers * kPerProducer);
    CHECK(orderOk);
    CHECK(box.empty());
    CHECK(box.dropped() == 0);
    // The consumer was woken at least once, and far fewer times than there were messages.
    CHECK(wakes.load() >= 1);
    CHECK(wakes.load() < kProducers * kPerProducer);
}

TEST_CASE("ui mailbox: concurrent token producers lose no text") {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 3000;
    // Tokens of different requests do not merge with each other, so the queue can hold many
    // messages if the consumer is slow: give it room, the limit is not what is tested here.
    const auto box = makeUiMailbox({}, static_cast<std::size_t>(kThreads) * kPerThread);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&box, t] {
            for (int i = 0; i < kPerThread; ++i) {
                box->push(token(static_cast<std::uint64_t>(t) + 1, "x"));
            }
        });
    }
    std::vector<std::size_t> perGeneration(kThreads + 1, 0);
    std::atomic<bool> done{false};
    std::thread consumer([&] {
        while (!done.load()) {
            for (const UiMessage& message : box->drain()) {
                const auto& t = std::get<ProviderToken>(message);
                perGeneration[static_cast<std::size_t>(t.generation)] += t.text.size();
            }
            std::this_thread::yield();
        }
    });
    for (std::thread& thread : threads) {
        thread.join();
    }
    done = true;
    consumer.join();
    for (const UiMessage& message : box->drain()) {
        const auto& t = std::get<ProviderToken>(message);
        perGeneration[static_cast<std::size_t>(t.generation)] += t.text.size();
    }
    for (int t = 1; t <= kThreads; ++t) {
        CHECK(perGeneration[static_cast<std::size_t>(t)] == static_cast<std::size_t>(kPerThread));
    }
    CHECK(box->dropped() == 0);
}
