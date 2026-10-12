// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "http/anthropic_provider.h"
#include "silent_server.h"

using namespace std::chrono_literals;
using curia::SecretString;
using curia::http::AnthropicProvider;
using curia::http::AnthropicProviderOptions;
using curia::http::AuthStyle;
using curia::http::makeAnthropicHeaders;
using curia::net::Endpoint;
using curia::providers::CancelToken;
using curia::providers::ChatError;
using curia::providers::ChatRequest;
using curia::providers::Role;
using curia::providers::StreamHandlers;
using curia::providers::Usage;

namespace {

const std::string kKeyText = std::string("sk-ant-") + "FAKE" + "-provider-unit-0123";

std::shared_ptr<const SecretString> keyForHeaders() {
    return std::make_shared<const SecretString>(std::string(kKeyText));
}

SecretString keyForProvider() {
    return SecretString(std::string(kKeyText));
}

Endpoint testEndpoint(int port) {
    const std::string host = "127.0.0.1";
    return Endpoint{"http://" + host + ":" + std::to_string(port) + "/v1/messages", true, true,
                    host};
}

ChatRequest oneQuestion() {
    ChatRequest request;
    request.model = "mock-model-1";
    request.conversation.push_back({Role::User, "Hello"});
    return request;
}

// Collects the single terminal call of a request.
struct Outcome {
    std::promise<ChatError> error;
    std::promise<Usage> done;
    std::atomic<int> terminals{0};
    std::atomic<int> tokens{0};
    std::thread::id errorThread;

    StreamHandlers handlers() {
        StreamHandlers handlers;
        handlers.onToken = [this](curia::providers::TokenKind, std::string_view) { ++tokens; };
        handlers.onDone = [this](const Usage& usage) {
            ++terminals;
            done.set_value(usage);
        };
        handlers.onError = [this](const ChatError& failure) {
            ++terminals;
            errorThread = std::this_thread::get_id();
            error.set_value(failure);
        };
        return handlers;
    }
};

}  // namespace

TEST_CASE("anthropic-provider: headers for Bearer and x-api-key") {
    const auto bearer = makeAnthropicHeaders(AuthStyle::Bearer, keyForHeaders());
    bool sawAuth = false;
    bool sawVersion = false;
    bool sawType = false;
    bool sawAccept = false;
    for (const auto& header : bearer) {
        CHECK(header.value.find(kKeyText) == std::string::npos);  // the key is never a plain value
        if (header.name == "authorization") {
            sawAuth = true;
            CHECK(header.value == "Bearer ");
            CHECK(header.secret != nullptr);
        } else {
            CHECK(header.secret == nullptr);
        }
        sawVersion = sawVersion || (header.name == "anthropic-version" &&
                                    header.value == std::string(curia::http::kAnthropicVersion));
        sawType = sawType || (header.name == "content-type" && header.value == "application/json");
        sawAccept = sawAccept || (header.name == "accept" && header.value == "text/event-stream");
    }
    CHECK(sawAuth);
    CHECK(sawVersion);
    CHECK(sawType);
    CHECK(sawAccept);

    const auto legacy = makeAnthropicHeaders(AuthStyle::XApiKey, keyForHeaders());
    int authHeaders = 0;
    for (const auto& header : legacy) {
        CHECK(header.name != "authorization");
        if (header.name == "x-api-key") {
            ++authHeaders;
            CHECK(header.value.empty());
            CHECK(header.secret != nullptr);
        }
    }
    CHECK(authHeaders == 1);
}

TEST_CASE("anthropic-provider: no key, nothing is sent and the error comes from the net thread") {
    AnthropicProvider provider(testEndpoint(1), SecretString{});
    Outcome outcome;
    auto future = outcome.error.get_future();
    provider.streamChat(oneQuestion(), outcome.handlers(), CancelToken{});
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    const ChatError error = future.get();
    CHECK((error.code == ChatError::Code::Auth));
    CHECK_FALSE(error.retryable);
    CHECK(outcome.errorThread != std::this_thread::get_id());
    CHECK(outcome.terminals.load() == 1);
}

TEST_CASE("anthropic-provider: a conversation that does not end with the player is a bad request") {
    AnthropicProvider provider(testEndpoint(1), keyForProvider());
    ChatRequest request = oneQuestion();
    request.conversation.push_back({Role::Assistant, "An answer"});
    Outcome outcome;
    auto future = outcome.error.get_future();
    provider.streamChat(request, outcome.handlers(), CancelToken{});
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    const ChatError error = future.get();
    CHECK((error.code == ChatError::Code::BadRequest));
    CHECK_FALSE(error.message.empty());
    CHECK(error.message.find(kKeyText) == std::string::npos);
}

TEST_CASE("anthropic-provider: an endpoint that cannot take the key stops the request") {
    AnthropicProvider provider(Endpoint{}, keyForProvider());
    Outcome outcome;
    auto future = outcome.error.get_future();
    provider.streamChat(oneQuestion(), outcome.handlers(), CancelToken{});
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    const ChatError error = future.get();
    CHECK((error.code == ChatError::Code::Other));
    CHECK_FALSE(error.retryable);
}

TEST_CASE("anthropic-provider: cancelled before the start ends as Cancelled with no text") {
    AnthropicProvider provider(testEndpoint(1), keyForProvider());
    Outcome outcome;
    auto future = outcome.error.get_future();
    CancelToken token;
    token.cancel();
    provider.streamChat(oneQuestion(), outcome.handlers(), token);
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    const ChatError error = future.get();
    CHECK((error.code == ChatError::Code::Cancelled));
    CHECK(error.partialText.empty());
    CHECK_FALSE(error.retryable);
    CHECK(outcome.tokens.load() == 0);
}

#if defined(CURIA_ENABLE_DEV_ENV)

TEST_CASE("anthropic-provider: a refused connection is a retryable network error") {
    AnthropicProvider provider(testEndpoint(1), keyForProvider());
    Outcome outcome;
    auto future = outcome.error.get_future();
    provider.streamChat(oneQuestion(), outcome.handlers(), CancelToken{});
    REQUIRE(future.wait_for(10s) == std::future_status::ready);
    const ChatError error = future.get();
    CHECK((error.code == ChatError::Code::Network));
    CHECK(error.retryable);
    CHECK(error.httpStatus == 0);
    CHECK(error.message.find(kKeyText) == std::string::npos);
    CHECK(outcome.terminals.load() == 1);
}

TEST_CASE("anthropic-provider: the connection's numbers are reported once per request") {
    std::promise<curia::http::NetSummary> summary;
    std::atomic<int> calls{0};
    AnthropicProviderOptions options;
    options.onNetDone = [&](const curia::http::NetSummary& value) {
        if (++calls == 1) {
            summary.set_value(value);
        }
    };
    AnthropicProvider provider(testEndpoint(1), keyForProvider(), options);
    Outcome outcome;
    auto future = outcome.error.get_future();
    provider.streamChat(oneQuestion(), outcome.handlers(), CancelToken{});
    REQUIRE(future.wait_for(10s) == std::future_status::ready);
    auto netFuture = summary.get_future();
    REQUIRE(netFuture.wait_for(10s) == std::future_status::ready);
    const curia::http::NetSummary net = netFuture.get();
    CHECK(net.httpStatus == 0);
    CHECK(net.curlCode != 0);
    CHECK_FALSE(net.tlsRetried);
    CHECK(net.connectMs == -1);
    CHECK(calls.load() == 1);
}

#if !defined(_WIN32)

TEST_CASE("anthropic-provider: destroying the provider mid-request calls no handler and closes the "
          "socket") {
    curia::testing::SilentServer server;
    REQUIRE(server.ok());
    Outcome outcome;
    {
        AnthropicProvider provider(testEndpoint(server.port()), keyForProvider());
        provider.streamChat(oneQuestion(), outcome.handlers(), CancelToken{});
        REQUIRE(server.waitAccepted(5s));
    }
    CHECK(outcome.terminals.load() == 0);
    CHECK(server.waitPeerClosed(5s));
}

TEST_CASE("anthropic-provider: cancelling in flight reports Cancelled once and closes the socket") {
    curia::testing::SilentServer server;
    REQUIRE(server.ok());
    AnthropicProvider provider(testEndpoint(server.port()), keyForProvider());
    Outcome outcome;
    auto future = outcome.error.get_future();
    CancelToken token;
    provider.streamChat(oneQuestion(), outcome.handlers(), token);
    REQUIRE(server.waitAccepted(5s));
    token.cancel();
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    CHECK((future.get().code == ChatError::Code::Cancelled));
    CHECK(server.waitPeerClosed(5s));
    CHECK(outcome.terminals.load() == 1);
}

TEST_CASE(
    "anthropic-provider: cancel from another thread reports at once, not after a poll timeout") {
    curia::testing::SilentServer server;
    REQUIRE(server.ok());
    AnthropicProvider provider(testEndpoint(server.port()), keyForProvider());
    Outcome outcome;
    auto future = outcome.error.get_future();
    CancelToken token;
    provider.streamChat(oneQuestion(), outcome.handlers(), token);
    REQUIRE(server.waitAccepted(5s));
    std::this_thread::sleep_for(100ms);
    const auto cancelledAt = std::chrono::steady_clock::now();
    token.cancel();
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    CHECK(std::chrono::steady_clock::now() - cancelledAt < 400ms);
    CHECK((future.get().code == ChatError::Code::Cancelled));
}

TEST_CASE("anthropic-provider: a handler that drops the last owner of the provider is safe") {
    curia::testing::SilentServer server;
    REQUIRE(server.ok());
    auto provider =
        std::make_shared<AnthropicProvider>(testEndpoint(server.port()), keyForProvider());
    std::promise<ChatError> errored;
    auto future = errored.get_future();
    std::atomic<int> terminals{0};
    StreamHandlers handlers;
    handlers.onError = [&errored, &terminals, owner = provider](const ChatError& error) {
        ++terminals;
        errored.set_value(error);
        // `owner` is released when the handlers are destroyed on the net thread, which destroys the
        // provider and its client there.
    };
    CancelToken token;
    provider->streamChat(oneQuestion(), std::move(handlers), token);
    REQUIRE(server.waitAccepted(5s));
    provider.reset();
    token.cancel();
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    CHECK((future.get().code == ChatError::Code::Cancelled));
    CHECK(server.waitPeerClosed(5s));
    std::this_thread::sleep_for(200ms);  // the net thread finishes on its own
    CHECK(terminals.load() == 1);
}

#endif  // !_WIN32

#endif  // CURIA_ENABLE_DEV_ENV
