// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "providers/chat_types.h"

namespace curia::providers {

struct BuiltRequest {
    // The JSON body of POST /v1/messages; compact, deterministic, always valid UTF-8 JSON.
    std::string body;
    // Empty when the conversation can be sent. Otherwise why it cannot (it does not end with a
    // user message); the body is still produced so that a caller may log its shape, but sending it
    // would be a 400. A plain English sentence for the log, not for the player.
    std::string problem;
};

// Builds the Messages request body for `request` (docs/architecture.md section 6.2, research-notes
// C13). Pure: no I/O, no clock, no randomness; the same request gives the same bytes.
//
// Keys: model, max_tokens, stream (true), and system when there is any system text, and messages.
// output_config {effort: <word>} only when request.effort is set, and thinking {type: <word>}
// only when request.thinking is set, each with that one member and nothing else (no budget:
// budget_tokens is a 400 on the current models). Never any of temperature, top_p, top_k,
// tool_choice, metadata (the current models answer 400 to forced or non-default values, and the
// defaults are what Curia wants). With both settings at Default the bytes are those of a request
// without them (ChatRequest defaults to Default; the app's own choice is providers::kDefaultEffort,
// D22).
//
// system is an array of text blocks in a fixed order: block 1 the persona, block 2 the realm
// snapshot. A block carries cache_control {type: ephemeral} (plus ttl "1h" when cache.longTtl)
// only if its own CacheHints flag is set, so the cached prefix is the persona alone, or the
// persona and the snapshot. A persona or snapshot that is empty or only white space is left out
// altogether: the API answers 400 to an empty text block. If both are left out there is no system
// key. Because the order in the API's cache is tools, system, messages, the bytes of `system` do
// not depend on the conversation, which is what keeps follow-up questions in the cache.
//
// messages hold the conversation, with these rules so that the API's own (strict) rules are met:
//  - Role::System messages are skipped: the persona is the only system text, and the Messages API
//    has no system role in messages.
//  - Messages whose text is empty or only white space are skipped (the API rejects them).
//  - Leading assistant messages are skipped (the first message must be from the user).
//  - Consecutive messages of one role are merged into one, joined by a blank line, so that the
//    roles alternate.
// If the result does not end with a user message `problem` says so.
//
// Text that is not valid UTF-8 (for example the cut-off partial answer of a "continue" turn) is
// written with U+FFFD in place of each bad byte instead of failing; nothing throws.
BuiltRequest buildAnthropicRequest(const ChatRequest& request);

// Just the body of buildAnthropicRequest().
std::string buildAnthropicRequestJson(const ChatRequest& request);

}  // namespace curia::providers
