// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "providers/chat_types.h"

namespace curia::advisor {

// What the player is told about a failed request: plain wording chosen by describeChatError()
// (chat_error_text.h), plus the already-redacted server message for the curious.
struct ErrorInfo {
    providers::ChatError::Code code = providers::ChatError::Code::Other;
    // One or two plain sentences; never a header, a key or a request body.
    std::string summary;
    // The provider's own (redacted, bounded) message; may be empty.
    std::string detail;
    bool retryable = false;
    int httpStatus = 0;
    std::optional<int> retryAfterSeconds;
    std::string requestId;
};

enum class EntryKind { User, Assistant, Divider };

// How an answer ended. Pending = still being produced.
enum class AnswerEnd { Pending, Complete, Stopped, Failed };

// One row of the chat as the player sees it. A Divider has only text ("New snapshot: ...").
struct TranscriptEntry {
    EntryKind kind = EntryKind::User;
    std::string text;
    // Assistant entries only.
    AnswerEnd end = AnswerEnd::Pending;
    // The answer was cut off by the token limit (a normal end that the player should see).
    bool hitTokenLimit = false;
    std::optional<ErrorInfo> error;
    providers::Usage usage;
    // Milliseconds from the ask to the first answer text and to the end, as the UI thread saw
    // them; -1 while unknown.
    long long firstTokenMs = -1;
    long long totalMs = -1;
};

}  // namespace curia::advisor
