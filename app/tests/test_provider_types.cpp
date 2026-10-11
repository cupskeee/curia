// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <atomic>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "providers/cancel_token.h"
#include "providers/chat_types.h"
#include "providers/provider.h"

using curia::providers::CancelToken;
using curia::providers::ChatError;
using curia::providers::ChatRequest;
using curia::providers::Provider;
using curia::providers::StreamHandlers;
using curia::providers::TokenKind;
using curia::providers::Usage;

// --- plain data ---------------------------------------------------------------------------------

TEST_CASE("provider types: defaults") {
    const ChatRequest request;
    CHECK(request.maxTokens == 4096);
    CHECK_FALSE(request.showThinking);
    CHECK(request.cache.cacheSystem);
    CHECK(request.cache.cacheSnapshot);
    CHECK_FALSE(request.cache.longTtl);
    CHECK(request.model.empty());
    CHECK(request.conversation.empty());

    const Usage usage;
    CHECK(usage.input == 0);
    CHECK(usage.output == 0);
    CHECK(usage.cacheRead == 0);
    CHECK(usage.cacheWrite == 0);
    CHECK(usage.stopReason.empty());

    const ChatError error;
    CHECK((error.code == ChatError::Code::Other));
    CHECK_FALSE(error.retryable);
    CHECK(error.httpStatus == 0);
    CHECK_FALSE(error.retryAfterSeconds.has_value());
    CHECK(error.requestId.empty());
    CHECK(error.partialText.empty());
}

TEST_CASE("provider types: every error code has its own name") {
    using Code = ChatError::Code;
    const std::vector<Code> codes = {Code::Auth,    Code::RateLimit, Code::Overloaded,
                                     Code::Network, Code::Cancelled, Code::BadRequest,
                                     Code::Policy,  Code::Other};
    std::set<std::string_view> names;
    for (const Code code : codes) {
        const std::string_view name = curia::providers::toString(code);
        CHECK_FALSE(name.empty());
        names.insert(name);
    }
    CHECK(names.size() == codes.size());
    CHECK(std::string(curia::providers::toString(Code::RateLimit)) == "RateLimit");
}

TEST_CASE("provider types: the handlers are callable and start empty") {
    StreamHandlers handlers;
    CHECK_FALSE(handlers.onToken);
    CHECK_FALSE(handlers.onDone);
    CHECK_FALSE(handlers.onError);

    std::string text;
    int done = 0;
    handlers.onToken = [&text](TokenKind kind, std::string_view piece) {
        if (kind == TokenKind::Text) {
            text += piece;
        }
    };
    handlers.onDone = [&done](const Usage& usage) { done += usage.output; };
    handlers.onToken(TokenKind::Text, "a");
    handlers.onToken(TokenKind::Thinking, "hidden");
    handlers.onToken(TokenKind::Text, "b");
    Usage usage;
    usage.output = 3;
    handlers.onDone(usage);
    CHECK(text == "ab");
    CHECK(done == 3);
}

namespace {

// Enough of a provider to prove the interface can be implemented and used through a pointer.
class FakeProvider final : public Provider {
public:
    void streamChat(const ChatRequest& request, StreamHandlers handlers,
                    CancelToken cancel) override {
        if (cancel.cancelled()) {
            ChatError error;
            error.code = ChatError::Code::Cancelled;
            handlers.onError(error);
            return;
        }
        handlers.onToken(TokenKind::Text, request.model);
        handlers.onDone(Usage{});
    }
};

}  // namespace

TEST_CASE("provider types: Provider can be implemented and called through the base class") {
    FakeProvider fake;
    Provider& provider = fake;
    ChatRequest request;
    request.model = "mock-model-1";

    std::string text;
    bool done = false;
    StreamHandlers handlers;
    handlers.onToken = [&text](TokenKind, std::string_view piece) { text += piece; };
    handlers.onDone = [&done](const Usage&) { done = true; };
    handlers.onError = [](const ChatError&) {};
    provider.streamChat(request, handlers, CancelToken());
    CHECK(text == "mock-model-1");
    CHECK(done);

    CancelToken cancel;
    cancel.cancel();
    ChatError seen;
    handlers.onError = [&seen](const ChatError& error) { seen = error; };
    provider.streamChat(request, handlers, cancel);
    CHECK((seen.code == ChatError::Code::Cancelled));
}

// --- CancelToken ------------------------------------------------------------------------------

TEST_CASE("cancel token: starts uncancelled, cancel sets the flag") {
    CancelToken token;
    CHECK_FALSE(token.cancelled());
    token.cancel();
    CHECK(token.cancelled());
}

TEST_CASE("cancel token: cancel is idempotent and calls the waker once") {
    CancelToken token;
    int calls = 0;
    token.setWaker([&calls] { ++calls; });
    CHECK(calls == 0);
    token.cancel();
    token.cancel();
    token.cancel();
    CHECK(token.cancelled());
    CHECK(calls == 1);
}

TEST_CASE("cancel token: cancel without a waker is fine") {
    CancelToken token;
    token.cancel();
    token.cancel();
    CHECK(token.cancelled());
}

TEST_CASE("cancel token: a waker set after the cancel runs once, at once") {
    CancelToken token;
    token.cancel();
    int calls = 0;
    token.setWaker([&calls] { ++calls; });
    CHECK(calls == 1);
    token.cancel();
    CHECK(calls == 1);
}

TEST_CASE("cancel token: setting a waker twice replaces the first") {
    CancelToken token;
    int first = 0;
    int second = 0;
    token.setWaker([&first] { ++first; });
    token.setWaker([&second] { ++second; });
    token.cancel();
    CHECK(first == 0);
    CHECK(second == 1);
}

TEST_CASE("cancel token: replacing the waker after the cancel runs the new one") {
    CancelToken token;
    int first = 0;
    int second = 0;
    token.setWaker([&first] { ++first; });
    token.cancel();
    token.setWaker([&second] { ++second; });
    CHECK(first == 1);
    CHECK(second == 1);
}

TEST_CASE("cancel token: an empty waker removes the waker") {
    CancelToken token;
    int calls = 0;
    token.setWaker([&calls] { ++calls; });
    token.setWaker({});
    token.cancel();
    CHECK(calls == 0);
}

TEST_CASE("cancel token: copies share one state") {
    const CancelToken original;
    CancelToken copy = original;
    CancelToken assigned;
    assigned = original;
    int calls = 0;
    copy.setWaker([&calls] { ++calls; });
    CHECK_FALSE(copy.cancelled());
    assigned.cancel();
    CHECK(original.cancelled());
    CHECK(copy.cancelled());
    CHECK(calls == 1);
    // A token made on its own is independent.
    CHECK_FALSE(CancelToken().cancelled());
}

TEST_CASE("cancel token: the waker may use the token without deadlock") {
    CancelToken token;
    int inner = 0;
    bool sawCancelled = false;
    token.setWaker([&] {
        sawCancelled = token.cancelled();
        token.cancel();                         // idempotent, from inside the waker
        token.setWaker([&inner] { ++inner; });  // already cancelled, so this one runs at once
    });
    token.cancel();
    CHECK(sawCancelled);
    CHECK(inner == 1);
}

namespace {

// Starts `count` threads that all wait for a common signal, then run `body(index)`.
template <typename Body> void runTogether(int count, Body body) {
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&go, &body, i] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            body(i);
        });
    }
    go.store(true);
    for (std::thread& thread : threads) {
        thread.join();
    }
}

}  // namespace

TEST_CASE("cancel token: concurrent cancels call the waker exactly once") {
    for (int round = 0; round < 200; ++round) {
        CancelToken token;
        std::atomic<int> calls{0};
        token.setWaker([&calls] { calls.fetch_add(1); });
        runTogether(8, [&token](int) { token.cancel(); });
        INFO("round " << round);
        REQUIRE(calls.load() == 1);
        REQUIRE(token.cancelled());
    }
}

TEST_CASE("cancel token: a cancel racing a setWaker still calls the waker exactly once") {
    for (int round = 0; round < 500; ++round) {
        CancelToken token;
        std::atomic<int> calls{0};
        runTogether(4, [&](int index) {
            if (index % 2 == 0) {
                token.cancel();
            } else {
                token.setWaker([&calls] { calls.fetch_add(1); });
            }
        });
        INFO("round " << round);
        // Two threads set a waker (the second replaces the first), two cancel. Each installation
        // that saw the cancel ran once; there is never a second call for one installation.
        REQUIRE(token.cancelled());
        REQUIRE(calls.load() >= 1);
        REQUIRE(calls.load() <= 2);
    }
}

TEST_CASE("cancel token: one waker raced against one cancel runs exactly once") {
    for (int round = 0; round < 500; ++round) {
        CancelToken token;
        std::atomic<int> calls{0};
        runTogether(2, [&](int index) {
            if (index == 0) {
                token.cancel();
            } else {
                token.setWaker([&calls] { calls.fetch_add(1); });
            }
        });
        INFO("round " << round);
        REQUIRE(calls.load() == 1);
    }
}
