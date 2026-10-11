// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The vocabulary shared by every provider (docs/architecture.md section 6): what is asked, the
// callbacks the answer arrives through, and how a failure is described. Pure data, no I/O.
namespace curia::providers {

enum class Role { System, User, Assistant };

struct Message {
    Role role = Role::User;
    std::string text;
};

// Honoured by the Anthropic provider, ignored elsewhere.
struct CacheHints {
    // A breakpoint after the stable persona block.
    bool cacheSystem = true;
    // A breakpoint after the realm snapshot block.
    bool cacheSnapshot = true;
    // 1 hour instead of 5 minutes.
    bool longTtl = false;
};

struct ChatRequest {
    // From the provider's models endpoint or free text (D4); never a built-in literal.
    std::string model;
    // Stable between requests.
    std::string systemPersona;
    // Changes with each export.
    std::string snapshotText;
    // The user and assistant turns, oldest first.
    std::vector<Message> conversation;
    // Thinking tokens count toward it on current models.
    int maxTokens = 4096;
    bool showThinking = false;
    CacheHints cache;
};

enum class TokenKind { Text, Thinking };

struct Usage {
    int input = 0;
    int output = 0;
    int cacheRead = 0;
    int cacheWrite = 0;
    // The provider's stop reason word as received ("end_turn", "max_tokens", ...); empty if none.
    // An answer cut off by the token limit is a normal onDone whose stopReason is "max_tokens";
    // the caller decides how to show that.
    std::string stopReason;
};

struct ChatError {
    enum class Code { Auth, RateLimit, Overloaded, Network, Cancelled, BadRequest, Policy, Other };

    Code code = Code::Other;
    // For the log and the status line; never the request, a header or the key.
    std::string message;
    // Offering a retry makes sense.
    bool retryable = false;
    // 0 when no HTTP response was received.
    int httpStatus = 0;
    // From the Retry-After header, in seconds, when the server sent one.
    std::optional<int> retryAfterSeconds;
    // The provider's request id, for support requests; empty if none.
    std::string requestId;
    // The text received before the failure (kept for display); empty if none.
    std::string partialText;
};

// A static string, like the other toString() functions of the code base. (Tests: doctest prints an
// operand through an unqualified toString(), so compare codes as CHECK((a == b)), as elsewhere.)
constexpr const char* toString(ChatError::Code code) noexcept {
    switch (code) {
    case ChatError::Code::Auth:
        return "Auth";
    case ChatError::Code::RateLimit:
        return "RateLimit";
    case ChatError::Code::Overloaded:
        return "Overloaded";
    case ChatError::Code::Network:
        return "Network";
    case ChatError::Code::Cancelled:
        return "Cancelled";
    case ChatError::Code::BadRequest:
        return "BadRequest";
    case ChatError::Code::Policy:
        return "Policy";
    case ChatError::Code::Other:
        return "Other";
    }
    return "Unknown";
}

// A progress marker between the request and the first visible token, for the status line.
enum class StreamStatus { ThinkingStarted, TextStarted };

// All of them run on the provider's reader thread and must not throw. Exactly one of onDone and
// onError ends a request; nothing follows it. onStatus is optional (may be empty); a provider
// checks it before calling.
struct StreamHandlers {
    std::function<void(TokenKind, std::string_view)> onToken;
    std::function<void(const Usage&)> onDone;
    std::function<void(const ChatError&)> onError;
    std::function<void(StreamStatus)> onStatus;
};

}  // namespace curia::providers
