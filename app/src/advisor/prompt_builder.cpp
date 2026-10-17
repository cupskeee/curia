// SPDX-License-Identifier: MIT
#include "advisor/prompt_builder.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "advisor/persona.h"
#include "advisor/snapshot_renderer.h"

namespace curia::advisor {

namespace {

bool isBlank(const std::string& text) {
    return std::ranges::all_of(
        text, [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
}

struct Pair {
    const TranscriptEntry* question = nullptr;
    const TranscriptEntry* answer = nullptr;
};

// The complete pairs among transcript[0 .. end), oldest first.
std::vector<Pair> completePairs(const std::vector<TranscriptEntry>& transcript, std::size_t end) {
    std::vector<Pair> pairs;
    for (std::size_t i = 0; i < end; ++i) {
        const TranscriptEntry& entry = transcript[i];
        if (entry.kind != EntryKind::User || isBlank(entry.text)) {
            continue;
        }
        // The answer is the next entry that is not a divider, if it is an assistant entry.
        std::size_t j = i + 1;
        while (j < end && transcript[j].kind == EntryKind::Divider) {
            ++j;
        }
        if (j < end && transcript[j].kind == EntryKind::Assistant && !isBlank(transcript[j].text)) {
            pairs.push_back({&entry, &transcript[j]});
        }
    }
    return pairs;
}

}  // namespace

std::optional<providers::ChatRequest> buildChatRequest(const PromptInput& input) {
    if (input.transcript == nullptr || input.questionIndex >= input.transcript->size()) {
        return std::nullopt;
    }
    const std::vector<TranscriptEntry>& transcript = *input.transcript;
    const TranscriptEntry& question = transcript[input.questionIndex];
    if (question.kind != EntryKind::User || isBlank(question.text)) {
        return std::nullopt;
    }

    providers::ChatRequest request;
    request.model = input.settings.model;
    request.maxTokens = input.settings.maxTokens;
    request.effort = input.settings.effort;
    request.thinking = input.settings.thinking;
    request.showThinking = input.settings.showThinking;
    request.cache = input.settings.cache;
    request.systemPersona = std::string(persona());
    request.snapshotText =
        input.snapshotText.empty() ? renderNoSnapshot() : std::string(input.snapshotText);

    const std::vector<Pair> pairs = completePairs(transcript, input.questionIndex);
    const std::size_t first = pairs.size() > kMaxHistoryPairs ? pairs.size() - kMaxHistoryPairs : 0;
    for (std::size_t i = first; i < pairs.size(); ++i) {
        request.conversation.push_back({providers::Role::User, pairs[i].question->text});
        request.conversation.push_back({providers::Role::Assistant, pairs[i].answer->text});
    }

    std::string latest = question.text;
    const std::string note = freshnessNote(input.freshness);
    if (!note.empty()) {
        latest += "\n\n";
        latest += note;
    }
    request.conversation.push_back({providers::Role::User, std::move(latest)});
    return request;
}

}  // namespace curia::advisor
