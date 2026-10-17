// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <initializer_list>
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

// How much the model may think before it answers (the Messages API's output_config.effort).
// Default sends nothing, so the model's own default applies. Which values a model accepts is the
// API's business: an unsupported one is a 400, never a choice made here (D4: no model-specific
// rules in the app).
enum class Effort { Default, Low, Medium, High, XHigh, Max };

// The effort Curia asks for unless told otherwise (D22, owner, 2026-10-17, from the M4 quality
// check round 2: the first word after about 1 s instead of 6 s and about 60% fewer output tokens,
// rated as good as the default by the owner, average 4.4 against 4.6; the model can still think on
// harder questions, as it did on one question of five). The thinking setting
// stays Thinking::Default (not sent: the model's own adaptive thinking). The data types below keep
// Default as their own default, so a request built without a setting sends none; the places that
// stand for the app's choice (the controller's options, the prompt settings and the command
// line's options) start from this.
inline constexpr Effort kDefaultEffort = Effort::Low;

// The thinking mode (the Messages API's thinking.type). Default sends nothing (current models then
// think adaptively); "between_tools" and "disabled" are accepted only by some models, and only at
// some efforts (the API answers 400 otherwise).
enum class Thinking { Default, Adaptive, BetweenTools, Disabled };

// The wire words ("low", "medium", "high", "xhigh", "max"; "adaptive", "between_tools",
// "disabled"), and "default" for Default (a word for reports; it is never sent).
constexpr const char* toString(Effort effort) noexcept {
    switch (effort) {
    case Effort::Default:
        return "default";
    case Effort::Low:
        return "low";
    case Effort::Medium:
        return "medium";
    case Effort::High:
        return "high";
    case Effort::XHigh:
        return "xhigh";
    case Effort::Max:
        return "max";
    }
    return "default";
}

constexpr const char* toString(Thinking thinking) noexcept {
    switch (thinking) {
    case Thinking::Default:
        return "default";
    case Thinking::Adaptive:
        return "adaptive";
    case Thinking::BetweenTools:
        return "between_tools";
    case Thinking::Disabled:
        return "disabled";
    }
    return "default";
}

// The value of a wire word; nullopt for anything else ("default" included: it is no wire word.
// The command line maps "--effort default" to Effort::Default itself).
constexpr std::optional<Effort> parseEffort(std::string_view word) noexcept {
    for (const Effort effort :
         {Effort::Low, Effort::Medium, Effort::High, Effort::XHigh, Effort::Max}) {
        if (word == toString(effort)) {
            return effort;
        }
    }
    return std::nullopt;
}

constexpr std::optional<Thinking> parseThinking(std::string_view word) noexcept {
    for (const Thinking thinking :
         {Thinking::Adaptive, Thinking::BetweenTools, Thinking::Disabled}) {
        if (word == toString(thinking)) {
            return thinking;
        }
    }
    return std::nullopt;
}

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
    // Sent only when not Default. The app's own choice is kDefaultEffort (D22); this member stays
    // Default, so a request that sets nothing sends nothing.
    Effort effort = Effort::Default;
    Thinking thinking = Thinking::Default;
    // Deliver the model's reasoning text as Thinking tokens (a display matter, not a request key).
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
