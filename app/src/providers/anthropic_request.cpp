// SPDX-License-Identifier: MIT
#include "providers/anthropic_request.h"

#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace curia::providers {

namespace {

using nlohmann::json;

// What the API counts as an empty text block: nothing, or only white space.
bool isBlank(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

json systemBlock(const std::string& text, bool cache, bool longTtl) {
    json block = {{"type", "text"}, {"text", text}};
    if (cache) {
        json control = {{"type", "ephemeral"}};
        if (longTtl) {
            control["ttl"] = "1h";
        }
        block["cache_control"] = std::move(control);
    }
    return block;
}

struct Turn {
    Role role;
    std::string text;
};

// The conversation reduced to strictly alternating user and assistant turns, starting with a user
// turn (see the header for the rules).
std::vector<Turn> normalizedTurns(const std::vector<Message>& conversation) {
    std::vector<Turn> turns;
    for (const Message& message : conversation) {
        if (message.role == Role::System || isBlank(message.text)) {
            continue;
        }
        if (turns.empty() && message.role == Role::Assistant) {
            continue;
        }
        if (!turns.empty() && turns.back().role == message.role) {
            turns.back().text += "\n\n";
            turns.back().text += message.text;
        } else {
            turns.push_back({message.role, message.text});
        }
    }
    return turns;
}

}  // namespace

BuiltRequest buildAnthropicRequest(const ChatRequest& request) {
    json body = json::object();
    body["model"] = request.model;
    body["max_tokens"] = request.maxTokens;
    body["stream"] = true;

    json system = json::array();
    if (!isBlank(request.systemPersona)) {
        system.push_back(
            systemBlock(request.systemPersona, request.cache.cacheSystem, request.cache.longTtl));
    }
    if (!isBlank(request.snapshotText)) {
        system.push_back(
            systemBlock(request.snapshotText, request.cache.cacheSnapshot, request.cache.longTtl));
    }
    if (!system.empty()) {
        body["system"] = std::move(system);
    }

    const std::vector<Turn> turns = normalizedTurns(request.conversation);
    json messages = json::array();
    for (const Turn& turn : turns) {
        messages.push_back(
            {{"role", turn.role == Role::User ? "user" : "assistant"}, {"content", turn.text}});
    }
    body["messages"] = std::move(messages);

    BuiltRequest built;
    // Compact (no indent), UTF-8 kept as it is, and a bad byte becomes U+FFFD instead of a throw.
    // nlohmann's objects are sorted maps, which makes the key order, and so the bytes, fixed.
    built.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
    if (turns.empty()) {
        built.problem = "the conversation has no user message to send";
    } else if (turns.back().role != Role::User) {
        built.problem = "the conversation does not end with a user message";
    }
    return built;
}

std::string buildAnthropicRequestJson(const ChatRequest& request) {
    return buildAnthropicRequest(request).body;
}

}  // namespace curia::providers
