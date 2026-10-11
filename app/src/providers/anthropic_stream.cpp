// SPDX-License-Identifier: MIT
#include "providers/anthropic_stream.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

#include "providers/chat_error.h"
#include "providers/provider_util.h"

namespace curia::providers {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxStopReasonBytes = 64;

bool isKnownEvent(std::string_view name) {
    return name == "message_start" || name == "content_block_start" ||
           name == "content_block_delta" || name == "message_delta" || name == "message_stop" ||
           name == "error";
}

// The string member `key` of an object, or nullptr if it is missing or not a string.
const std::string* stringMember(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return nullptr;
    }
    return it->get_ptr<const std::string*>();
}

// The object member `key`, or nullptr if it is missing or not an object.
const json* objectMember(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_object()) {
        return nullptr;
    }
    return &*it;
}

// A usage number: an integer from 0 up, clamped to the range of int. Negative, fractional (also
// 1e3, which the JSON parser reads as a float) and non-number values are absent.
std::optional<int> usageNumber(const json& usage, const char* key) {
    const auto it = usage.find(key);
    if (it == usage.end()) {
        return std::nullopt;
    }
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    if (it->is_number_unsigned()) {
        return static_cast<int>(std::min(it->get<std::uint64_t>(), kMax));
    }
    // The parser stores every non-negative integer as unsigned except the literal -0, so signed
    // storage holds -0 (zero) and negative numbers (absent).
    if (it->is_number_integer() && it->get<std::int64_t>() == 0) {
        return 0;
    }
    return std::nullopt;
}

}  // namespace

AnthropicStreamDecoder::AnthropicStreamDecoder(StreamHandlers handlers, bool showThinking)
    : handlers_(std::move(handlers)), showThinking_(showThinking) {}

void AnthropicStreamDecoder::onEvent(const SseEvent& event) {
    if (finished_) {
        return;
    }
    std::string_view name = event.event;
    // Ignored without looking at the data, so that a future event with any payload is harmless.
    const bool named = name != "message";
    if (named && !isKnownEvent(name)) {
        return;
    }
    const std::optional<json> parsed = parseBoundedJson(event.data);
    if (!parsed || !parsed->is_object()) {
        finishMalformed();
        return;
    }
    if (!named) {
        // No event names in this stream (a proxy dropped them): the JSON names its own type.
        const std::string* type = stringMember(*parsed, "type");
        if (type == nullptr || !isKnownEvent(*type)) {
            return;
        }
        name = *type;
    }

    bool ok = true;
    if (name == "message_start") {
        ok = handleMessageStart(*parsed);
    } else if (name == "content_block_start") {
        ok = handleContentBlockStart(*parsed);
    } else if (name == "content_block_delta") {
        ok = handleContentBlockDelta(*parsed);
    } else if (name == "message_delta") {
        ok = handleMessageDelta(*parsed);
    } else if (name == "message_stop") {
        finishDone();
    } else {  // "error"
        ok = handleError(*parsed);
    }
    if (!ok) {
        finishMalformed();
    }
}

void AnthropicStreamDecoder::onTransportEnd() {
    if (finished_) {
        return;
    }
    ChatError error;
    error.code = ChatError::Code::Network;
    error.message = "the connection ended before the answer was complete";
    error.retryable = true;
    finishError(std::move(error));
}

void AnthropicStreamDecoder::onHttpError(int status, std::string_view body,
                                         std::string_view retryAfter, std::string_view requestId) {
    if (finished_) {
        return;
    }
    finishError(mapHttpError(status, body, retryAfter, requestId));
}

bool AnthropicStreamDecoder::handleMessageStart(const json& data) {
    const json* message = objectMember(data, "message");
    if (message == nullptr) {
        return false;
    }
    if (const std::string* id = stringMember(*message, "id")) {
        messageId_ = printableExcerpt(*id, kMaxRequestIdBytes);
    }
    if (const json* usage = objectMember(*message, "usage")) {
        applyUsage(*usage);
    }
    return true;
}

bool AnthropicStreamDecoder::handleContentBlockStart(const json& data) const {
    const json* block = objectMember(data, "content_block");
    if (block == nullptr) {
        return false;
    }
    const std::string* type = stringMember(*block, "type");
    if (type == nullptr) {
        return false;
    }
    if (handlers_.onStatus) {
        if (*type == "thinking") {
            handlers_.onStatus(StreamStatus::ThinkingStarted);
        } else if (*type == "text") {
            handlers_.onStatus(StreamStatus::TextStarted);
        }
    }
    return true;
}

bool AnthropicStreamDecoder::handleContentBlockDelta(const json& data) {
    const json* delta = objectMember(data, "delta");
    if (delta == nullptr) {
        return false;
    }
    const std::string* type = stringMember(*delta, "type");
    if (type == nullptr) {
        return false;
    }
    if (*type == "text_delta") {
        const std::string* text = stringMember(*delta, "text");
        if (text == nullptr) {
            return false;
        }
        deliverText(*text);
    } else if (*type == "thinking_delta") {
        const std::string* thinking = stringMember(*delta, "thinking");
        if (thinking == nullptr) {
            return false;
        }
        if (showThinking_ && !thinking->empty() && handlers_.onToken) {
            handlers_.onToken(TokenKind::Thinking, *thinking);
        }
    }
    // signature_delta, input_json_delta and kinds we do not know carry nothing we show.
    return true;
}

bool AnthropicStreamDecoder::handleMessageDelta(const json& data) {
    const json* delta = objectMember(data, "delta");
    if (delta == nullptr) {
        return false;
    }
    if (const std::string* reason = stringMember(*delta, "stop_reason")) {
        usage_.stopReason = printableExcerpt(*reason, kMaxStopReasonBytes);
    }
    if (const json* usage = objectMember(data, "usage")) {
        applyUsage(*usage);
    }
    return true;
}

bool AnthropicStreamDecoder::handleError(const json& data) {
    const json* error = objectMember(data, "error");
    if (error == nullptr) {
        return false;
    }
    const std::string* type = stringMember(*error, "type");
    const std::string* message = stringMember(*error, "message");
    ChatError mapped =
        mapStreamError(type != nullptr ? std::string_view(*type) : std::string_view(),
                       message != nullptr ? std::string_view(*message) : std::string_view());
    // The API repeats its request id at the top level of an error event.
    if (const std::string* requestId = stringMember(data, "request_id")) {
        mapped.requestId = printableExcerpt(*requestId, kMaxRequestIdBytes);
    }
    finishError(std::move(mapped));
    return true;
}

// Each field present replaces the stored value; the stream's numbers are cumulative, so the last
// one seen is the total. An absent or unusable field leaves the earlier value alone.
void AnthropicStreamDecoder::applyUsage(const json& usage) {
    if (const auto value = usageNumber(usage, "input_tokens")) {
        usage_.input = *value;
    }
    if (const auto value = usageNumber(usage, "output_tokens")) {
        usage_.output = *value;
    }
    if (const auto value = usageNumber(usage, "cache_read_input_tokens")) {
        usage_.cacheRead = *value;
    }
    if (const auto value = usageNumber(usage, "cache_creation_input_tokens")) {
        usage_.cacheWrite = *value;
    }
}

void AnthropicStreamDecoder::deliverText(const std::string& piece) {
    if (piece.empty()) {
        return;
    }
    if (text_.size() + piece.size() > kMaxAnswerBytes) {
        ChatError error;
        error.code = ChatError::Code::Other;
        error.message = "the answer is larger than expected";
        finishError(std::move(error));
        return;
    }
    text_ += piece;
    if (handlers_.onToken) {
        handlers_.onToken(TokenKind::Text, piece);
    }
}

void AnthropicStreamDecoder::finishDone() {
    if (usage_.stopReason == "refusal") {
        ChatError error;
        error.code = ChatError::Code::Policy;
        error.message = "the model declined to answer";
        finishError(std::move(error));
        return;
    }
    finished_ = true;
    if (handlers_.onDone) {
        handlers_.onDone(usage_);
    }
}

void AnthropicStreamDecoder::finishError(ChatError error) {
    // Set before the callback so that nothing can end the request twice, even from a handler.
    finished_ = true;
    error.partialText = text_;
    if (error.requestId.empty()) {
        error.requestId = messageId_;
    }
    if (handlers_.onError) {
        handlers_.onError(error);
    }
}

void AnthropicStreamDecoder::finishMalformed() {
    ChatError error;
    error.code = ChatError::Code::Other;
    error.message = "malformed event from the server";
    finishError(std::move(error));
}

}  // namespace curia::providers
