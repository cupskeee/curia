// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "advisor/freshness.h"
#include "advisor/transcript.h"
#include "providers/chat_types.h"

namespace curia::advisor {

// How many earlier question-and-answer pairs a request carries.
inline constexpr std::size_t kMaxHistoryPairs = 6;

struct PromptSettings {
    // From configuration or the provider's models endpoint; never a literal in source (D4).
    std::string model;
    // The answer limit and the thinking settings, passed to the request as they are (Default
    // effort and thinking send nothing).
    int maxTokens = 4096;
    providers::Effort effort = providers::Effort::Default;
    providers::Thinking thinking = providers::Thinking::Default;
    bool showThinking = false;
    providers::CacheHints cache;
};

struct PromptInput {
    PromptSettings settings;
    // The rendered snapshot block; empty means nothing has been read yet (renderNoSnapshot()).
    std::string_view snapshotText;
    // The chat so far. transcript[questionIndex] is the user turn to answer; entries after it
    // (a failed answer being retried, dividers) are ignored.
    const std::vector<TranscriptEntry>* transcript = nullptr;
    std::size_t questionIndex = 0;
    Freshness freshness;
};

// Builds the request:
//   * systemPersona = persona(); snapshotText = the rendered block. Neither holds anything that
//     varies between two questions about the same snapshot, so the provider's cache can reuse
//     the whole system prefix;
//   * the conversation is the last kMaxHistoryPairs complete pairs before the question, then the
//     question. A pair needs a non-blank question and a non-blank answer: an unanswered question,
//     a failed answer with no text and a divider are left out (an empty assistant turn is never
//     sent). A stopped or failed answer that has text is kept as it is;
//   * the freshness note (freshness.h) is appended to the LATEST user message only, after a blank
//     line, and only when it is not empty. History is never touched, so the bytes of every
//     earlier turn are the same in the next request.
// Returns nullopt when transcript is null, the index is out of range, or the entry there is not a
// user turn with text.
std::optional<providers::ChatRequest> buildChatRequest(const PromptInput& input);

}  // namespace curia::advisor
