// SPDX-License-Identifier: MIT
#include "providers/chat_error.h"

#include <cstddef>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "providers/provider_util.h"

namespace curia::providers {

namespace {

using Code = ChatError::Code;

struct Mapping {
    Code code = Code::Other;
    bool retryable = false;
};

// The error types of the API's error object; nullopt for a type we do not know.
std::optional<Mapping> mappingForType(std::string_view type) {
    if (type == "overloaded_error") {
        return Mapping{Code::Overloaded, true};
    }
    if (type == "rate_limit_error") {
        return Mapping{Code::RateLimit, true};
    }
    if (type == "authentication_error" || type == "permission_error") {
        return Mapping{Code::Auth, false};
    }
    if (type == "invalid_request_error" || type == "not_found_error" ||
        type == "request_too_large") {
        return Mapping{Code::BadRequest, false};
    }
    if (type == "api_error") {
        return Mapping{Code::Other, true};
    }
    if (type == "timeout_error") {
        return Mapping{Code::Network, true};
    }
    return std::nullopt;
}

Mapping mappingForStatus(int status) {
    switch (status) {
    case 401:
    case 403:
        return {Code::Auth, false};
    case 404:
        return {Code::BadRequest, false};
    case 408:
        return {Code::Network, true};
    case 429:
        return {Code::RateLimit, true};
    case 500:
        return {Code::Other, true};
    case 502:
    case 503:
    case 504:
        return {Code::Network, true};
    case 529:
        return {Code::Overloaded, true};
    default:
        break;
    }
    if (status >= 400 && status <= 499) {
        return {Code::BadRequest, false};  // 400, 413 and the rest: resending cannot help
    }
    if (status >= 500 && status <= 599) {
        return {Code::Other, true};
    }
    if (status >= 300 && status <= 399) {
        return {Code::Network, false};
    }
    return {Code::Other, false};
}

std::string describeStatus(int status) {
    const std::string http = "HTTP " + std::to_string(status);
    switch (status) {
    case 401:
        return "authentication failed (" + http + ")";
    case 403:
        return "access denied (" + http + ")";
    case 404:
        return "model not found: check the configured model (" + http + ")";
    case 408:
        return "the request timed out (" + http + ")";
    case 413:
        return "the request is too large (" + http + ")";
    case 429:
        return "rate limited (" + http + ")";
    case 529:
        return "the service is overloaded (" + http + ")";
    default:
        break;
    }
    if (status >= 300 && status <= 399) {
        return "unexpected redirect (" + http + ")";
    }
    if (status >= 400 && status <= 499) {
        return "the request was rejected (" + http + ")";
    }
    if (status >= 500 && status <= 599) {
        return "the service failed (" + http + ")";
    }
    return "unexpected response (" + http + ")";
}

std::string describeType(std::string_view type) {
    const std::string shown = printableExcerpt(type, 64);
    return shown.empty() ? "the service reported an error"
                         : "the service reported an error (" + shown + ")";
}

// Seconds from a Retry-After value of digits only, 0 to 86400.
std::optional<int> parseRetryAfter(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                              value.back() == '\n')) {
        value.remove_suffix(1);
    }
    constexpr std::size_t kMaxDigits = 6;  // 86400 has five; more cannot be in range
    if (value.empty() || value.size() > kMaxDigits) {
        return std::nullopt;
    }
    int seconds = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        seconds = seconds * 10 + (c - '0');
    }
    constexpr int kMaxSeconds = 86400;
    if (seconds > kMaxSeconds) {
        return std::nullopt;
    }
    return seconds;
}

// The pieces of the API's error body that we use.
struct ErrorBody {
    std::string type;
    std::string message;
};

std::optional<ErrorBody> parseErrorBody(std::string_view body) {
    const std::optional<nlohmann::json> parsed = parseBoundedJson(body);
    if (!parsed || !parsed->is_object()) {
        return std::nullopt;
    }
    const auto error = parsed->find("error");
    if (error == parsed->end() || !error->is_object()) {
        return std::nullopt;
    }
    ErrorBody out;
    if (const auto type = error->find("type"); type != error->end() && type->is_string()) {
        out.type = type->get<std::string>();
    }
    if (const auto message = error->find("message");
        message != error->end() && message->is_string()) {
        out.message = message->get<std::string>();
    }
    return out;
}

}  // namespace

ChatError mapStreamError(std::string_view type, std::string_view message) {
    const std::optional<Mapping> mapping = mappingForType(type);
    ChatError error;
    error.code = mapping ? mapping->code : Code::Other;
    error.retryable = mapping && mapping->retryable;
    error.message = printableExcerpt(message, kMaxServerMessageBytes);
    if (error.message.empty()) {
        error.message = describeType(type);
    }
    return error;
}

ChatError mapHttpError(int status, std::string_view body, std::string_view retryAfterHeader,
                       std::string_view requestId) {
    const Mapping byStatus = mappingForStatus(status);
    ChatError error;
    error.code = byStatus.code;
    error.retryable = byStatus.retryable;
    error.httpStatus = status;
    error.retryAfterSeconds = parseRetryAfter(retryAfterHeader);
    // From a response header, so as untrusted as the body: same bound and cleaning as the ids the
    // stream decoder takes from the body.
    error.requestId = printableExcerpt(requestId, kMaxRequestIdBytes);

    std::string serverMessage;
    const std::optional<ErrorBody> parsed = parseErrorBody(body);
    if (parsed) {
        if (const std::optional<Mapping> byType = mappingForType(parsed->type)) {
            error.code = byType->code;
            error.retryable = byType->retryable;
        }
        serverMessage = printableExcerpt(parsed->message, kMaxServerMessageBytes);
    }
    if (!serverMessage.empty()) {
        // A 404 is nearly always a wrong model name; say so, then what the server said.
        error.message = status == 404
                            ? "model not found: check the configured model (" + serverMessage + ")"
                            : serverMessage;
        return error;
    }
    // No usable message: an excerpt of whatever came back, or a description of the status.
    const std::string excerpt = printableExcerpt(body, kMaxBodyExcerptBytes);
    error.message = describeStatus(status);
    if (!excerpt.empty() && status != 404) {
        error.message = "HTTP " + std::to_string(status) + ": " + excerpt;
    } else if (!excerpt.empty()) {
        error.message += ": " + excerpt;
    }
    return error;
}

}  // namespace curia::providers
