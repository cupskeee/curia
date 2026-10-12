// SPDX-License-Identifier: MIT
#include "advisor/chat_error_text.h"

#include <string>

namespace curia::advisor {

namespace {

std::string retryWait(const std::optional<int>& seconds) {
    if (!seconds.has_value() || *seconds <= 0) {
        return "Try again in a moment.";
    }
    return "Try again in " + std::to_string(*seconds) + (*seconds == 1 ? " second." : " seconds.");
}

std::string summaryFor(const providers::ChatError& error) {
    using Code = providers::ChatError::Code;
    switch (error.code) {
    case Code::Auth:
        return "The service did not accept the API key. Check that the key is correct and still "
               "active.";
    case Code::Network:
        return "Could not reach the service. Check your internet connection and try again.";
    case Code::RateLimit:
        return "The service says there have been too many requests. " +
               retryWait(error.retryAfterSeconds);
    case Code::Overloaded:
        return "The service is busy right now. Try again in a moment.";
    case Code::BadRequest:
        return "The service refused the request. Check that the model name is right and try "
               "again.";
    case Code::Policy:
        return "The service declined to answer this one. Try asking in a different way.";
    case Code::Cancelled:
        return "Stopped.";
    case Code::Other:
        break;
    }
    return "Something went wrong on the service side. Try again in a moment.";
}

// Cuts at a character boundary: never leaves the start of a UTF-8 sequence without its end.
std::string capDetail(std::string text) {
    if (text.size() <= kMaxErrorDetailBytes) {
        return text;
    }
    std::size_t cut = kMaxErrorDetailBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
        --cut;
    }
    text.resize(cut);
    return text;
}

}  // namespace

ErrorInfo describeChatError(const providers::ChatError& error, const SecretRegistry* registry) {
    ErrorInfo info;
    info.code = error.code;
    info.summary = summaryFor(error);
    const std::string redacted =
        registry != nullptr ? registry->redact(error.message) : curia::redact(error.message);
    info.detail = capDetail(redacted);
    info.retryable = error.retryable;
    info.httpStatus = error.httpStatus;
    info.retryAfterSeconds = error.retryAfterSeconds;
    info.requestId =
        registry != nullptr ? registry->redact(error.requestId) : curia::redact(error.requestId);
    return info;
}

}  // namespace curia::advisor
