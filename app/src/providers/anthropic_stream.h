// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "providers/chat_types.h"
#include "providers/sse_parser.h"

namespace curia::providers {

// Turns the events of an Anthropic Messages stream (docs/research-notes.md section 2.7) into
// StreamHandlers calls. Pure: it is fed SseEvent values and, when the transport ends or fails,
// one of the two end calls; it never reads a socket, so it is tested and fuzzed without a network.
//
// What it does, by event (the SSE event name decides; if the stream carries no event names, the
// "type" of the JSON does):
//   message_start        remembers the message id and the usage numbers it holds
//   content_block_start  type "thinking" -> onStatus(ThinkingStarted), "text" -> TextStarted;
//                        other block types are ignored
//   content_block_delta  text_delta -> onToken(Text) and appended to text(); thinking_delta ->
//                        onToken(Thinking) only if showThinking; signature_delta,
//                        input_json_delta and unknown delta types are ignored
//   message_delta        the cumulative usage (each field present replaces the stored value, an
//                        absent field keeps it) and the stop reason
//   message_stop         ends the request: onDone(usage), or, if the stop reason is "refusal",
//                        onError(Policy) with the text so far (a stop reason of "max_tokens" is a
//                        normal onDone; the caller reads Usage::stopReason)
//   error                ends the request: onError(mapStreamError(...)) with the text so far
//   ping, content_block_stop and any other event are ignored without parsing their data.
// An event whose data is not valid JSON, nests deeper than kMaxJsonDepth, or lacks the fields its
// type needs ends the request with onError(Other, "malformed event from the server", not
// retryable). Token text is delivered as received, valid UTF-8 (a delta with invalid UTF-8 is a
// malformed event); an empty text delta delivers no token.
//
// Exactly one of onDone and onError is called, once, for the life of the decoder: whichever
// comes first among the events above, onTransportEnd() and onHttpError(). Everything after it is
// ignored. Usage numbers are clamped to [0, INT32_MAX]; a negative, fractional or non-number
// value counts as absent.
//
// Not thread-safe: all calls come from the provider's reader thread, and the handlers run there.
class AnthropicStreamDecoder {
public:
    // The most answer text kept (and delivered). Far above any real answer (max_tokens bounds
    // that); it only stops an endless stream from filling memory. Beyond it the request ends with
    // onError(Other).
    static constexpr std::size_t kMaxAnswerBytes = std::size_t{16} * 1024 * 1024;

    AnthropicStreamDecoder(StreamHandlers handlers, bool showThinking);

    void onEvent(const SseEvent& event);

    // The connection ended (cleanly or not) and no terminal event came: onError(Network,
    // retryable) with the text received. No effect once the request has ended.
    void onTransportEnd();

    // The server answered with an HTTP error instead of a stream: onError(mapHttpError(...)).
    // No effect once the request has ended.
    void onHttpError(int status, std::string_view body, std::string_view retryAfter,
                     std::string_view requestId);

    // True once onDone or onError has been called.
    bool finished() const noexcept { return finished_; }

    // The Text tokens delivered so far, concatenated (thinking is not included).
    const std::string& text() const noexcept { return text_; }

private:
    // Each handler returns false if the event lacks what its type needs (a malformed event).
    bool handleMessageStart(const nlohmann::json& data);
    bool handleContentBlockStart(const nlohmann::json& data) const;
    bool handleContentBlockDelta(const nlohmann::json& data);
    bool handleMessageDelta(const nlohmann::json& data);
    bool handleError(const nlohmann::json& data);
    void applyUsage(const nlohmann::json& usage);
    void deliverText(const std::string& piece);
    void finishDone();
    void finishError(ChatError error);
    void finishMalformed();

    StreamHandlers handlers_;
    bool showThinking_;
    bool finished_ = false;
    std::string text_;
    std::string messageId_;
    Usage usage_;
};

}  // namespace curia::providers
