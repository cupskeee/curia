// SPDX-License-Identifier: MIT
#include "http/anthropic_provider.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "core/build_info.h"
#include "core/redact.h"
#include "http/transport_error.h"
#include "providers/anthropic_request.h"
#include "providers/anthropic_stream.h"
#include "providers/chat_error.h"
#include "providers/sse_parser.h"

namespace curia::http {

using providers::AnthropicStreamDecoder;
using providers::ChatError;
using providers::ChatRequest;
using providers::SseEvent;
using providers::SseParser;
using providers::SseStatus;
using providers::StreamHandlers;
using providers::StreamStatus;
using providers::TokenKind;
using providers::Usage;

namespace {

using Clock = std::chrono::steady_clock;

// A non-2xx body is only read for the API's JSON error object, which is small.
constexpr std::size_t kMaxErrorBodyBytes = 32 * 1024;

std::int64_t millisBetween(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

bool isSuccess(int status) {
    return status >= 200 && status < 300;
}

// True if the first bytes of a 2xx body could begin a server-sent event stream: a field name
// ("event:", "data:", "id:", "retry:") or a comment (":"). Fewer than `kSniffBytes` bytes are
// compared as far as they go.
constexpr std::size_t kSniffBytes = 8;

bool couldBeEventStream(std::string_view head) {
    std::size_t first = 0;
    while (first < head.size() && (head[first] == ' ' || head[first] == '\t' ||
                                   head[first] == '\r' || head[first] == '\n')) {
        ++first;
    }
    head.remove_prefix(first);
    if (head.empty()) {
        return true;  // nothing but blanks so far
    }
    if (head.front() == ':') {
        return true;
    }
    for (const std::string_view field : {"event:", "data:", "id:", "retry:"}) {
        const std::size_t common = std::min(head.size(), field.size());
        if (head.substr(0, common) == field.substr(0, common)) {
            return true;
        }
    }
    return false;
}

const char* outcomeName(End end) {
    switch (end) {
    case End::Finished:
        return "finished";
    case End::Cancelled:
        return "cancelled";
    case End::Shutdown:
        return "shutdown";
    case End::Refused:
        return "refused";
    }
    return "unknown";
}

ChatError makeError(ChatError::Code code, std::string message, bool retryable) {
    ChatError error;
    error.code = code;
    error.message = std::move(message);
    error.retryable = retryable;
    return error;
}

// Everything one request needs from its first byte to its end. Shared by the three callbacks;
// they all run on the net thread, so it needs no lock.
struct StreamState {
    StreamHandlers user;
    std::shared_ptr<std::atomic<bool>> closing;
    AppLog* log = nullptr;
    std::function<void(const NetSummary&)> onNet;
    int stallSeconds = 45;
    SseParser parser;
    std::optional<AnthropicStreamDecoder> decoder;
    bool terminated = false;
    int status = 0;
    std::string requestId;
    std::string retryAfter;
    std::string errorBody;
    // The first bytes of a 2xx body, and how many events came out of it.
    std::string bodyHead;
    std::size_t events = 0;
    Clock::time_point start = Clock::now();
    std::optional<std::int64_t> firstTokenMs;

    bool muted() const { return closing->load(); }

    // What the decoder has delivered so far (the decoder exists from the moment the request is
    // made).
    std::string partialText() const {
        return decoder.has_value() ? decoder->text() : std::string();
    }

    void logLine(AppLog::Level level, std::string_view event,
                 std::initializer_list<AppLog::Field> fields) const {
        if (log != nullptr) {
            log->log(level, event, fields);
        }
    }

    void token(TokenKind kind, std::string_view text) {
        if (terminated || muted() || !user.onToken) {
            return;
        }
        if (kind == TokenKind::Text && !firstTokenMs.has_value()) {
            firstTokenMs = millisBetween(start, Clock::now());
        }
        // A registered key reflected by the server inside one token is replaced here. Shapes
        // (redact()) are deliberately not applied to a piece: the front of a key that continues in
        // the next token would match, be replaced alone, and leave the rest of the key in the
        // clear. A key split over two tokens passes as it is: the consumer that joins the tokens
        // redacts the joined text (redact()), and the final text (redactFinal()) cuts a key that
        // the end of the stream split. Every error field is redacted here as a whole.
        user.onToken(kind, redactRegistered(text));
    }

    void done(const Usage& usage) {
        if (terminated) {
            return;
        }
        terminated = true;
        logLine(AppLog::Level::Info, "chat_done",
                {{"input", usage.input},
                 {"output", usage.output},
                 {"cache_read", usage.cacheRead},
                 {"cache_write", usage.cacheWrite},
                 {"first_token_ms", firstTokenMs.value_or(-1)},
                 {"total_ms", millisBetween(start, Clock::now())},
                 {"stop_reason", usage.stopReason}});
        if (!muted() && user.onDone) {
            Usage safe = usage;
            safe.stopReason = redact(safe.stopReason);
            user.onDone(safe);
        }
    }

    void fail(ChatError error) {
        if (terminated) {
            return;
        }
        terminated = true;
        error.message = redact(error.message);
        error.partialText = redactFinal(error.partialText);
        if (error.requestId.empty()) {
            error.requestId = requestId;
        } else {
            error.requestId = redact(error.requestId);
        }
        logLine(AppLog::Level::Warn, "chat_error",
                {{"code", toString(error.code)},
                 {"retryable", error.retryable},
                 {"http_status", error.httpStatus},
                 {"partial_bytes", error.partialText.size()},
                 {"total_ms", millisBetween(start, Clock::now())}});
        if (!muted() && user.onError) {
            user.onError(error);
        }
    }

    bool onData(std::string_view bytes) {
        if (terminated) {
            return false;  // the answer is over; whatever else the server sends is not wanted
        }
        if (isSuccess(status)) {
            if (bodyHead.size() < kSniffBytes) {
                // Leading blanks do not count; the head is only used to tell a stream from a page.
                bodyHead.append(bytes.substr(0, kSniffBytes - bodyHead.size()));
            }
            const SseStatus parsed = parser.feed(bytes, [this](const SseEvent& event) {
                ++events;
                decoder->onEvent(event);
            });
            if (parsed != SseStatus::Ok) {
                ChatError error = makeError(ChatError::Code::Other,
                                            "the server sent an event that is too large", false);
                error.partialText = partialText();
                fail(std::move(error));
                return false;
            }
            return true;
        }
        const std::size_t room = kMaxErrorBodyBytes - errorBody.size();
        errorBody.append(bytes.substr(0, room));
        return errorBody.size() < kMaxErrorBodyBytes;
    }

    void onHead(const ResponseHead& head) {
        status = head.status;
        requestId = head.requestId;
        retryAfter = head.retryAfter;
    }

    void onComplete(const Result& result) {
        logLine(AppLog::Level::Info, "net_done",
                {{"outcome", outcomeName(result.end)},
                 {"status", result.httpStatus},
                 {"curl", result.curlCode},
                 {"bytes_in", result.bytesIn},
                 {"bytes_out", result.bytesOut},
                 {"connect_ms", result.timings.connectMs},
                 {"first_byte_ms", result.timings.firstByteMs},
                 {"total_ms", result.timings.totalMs},
                 {"tls_retry", result.tlsRetried}});
        if (onNet && !muted()) {
            NetSummary summary;
            summary.httpStatus = result.httpStatus;
            summary.tlsRetried = result.tlsRetried;
            summary.connectMs = result.timings.connectMs;
            summary.firstByteMs = result.timings.firstByteMs;
            summary.totalMs = result.timings.totalMs;
            summary.curlCode = result.curlCode;
            onNet(summary);
        }
        if (terminated) {
            return;
        }
        switch (result.end) {
        case End::Cancelled:
        case End::Shutdown: {
            ChatError error = makeError(ChatError::Code::Cancelled, "cancelled", false);
            error.partialText = partialText();
            fail(std::move(error));
            return;
        }
        case End::Refused:
            fail(makeError(ChatError::Code::Other, "the request was not sent: " + result.refusal,
                           false));
            return;
        case End::Finished:
            break;
        }
        if (status != 0 && !isSuccess(status)) {
            // A redirect is described by its status alone, whatever the body says: it is never
            // followed, and it must not be mistaken for an API error that is worth retrying.
            const bool redirect = status >= 300 && status < 400;
            fail(providers::mapHttpError(status, redirect ? std::string_view{} : errorBody,
                                         retryAfter, requestId));
            return;
        }
        if (result.curlCode != 0) {
            TransportContext context;
            context.connected = result.timings.connectMs >= 0;
            context.receivedAnything = status != 0 || result.bytesIn > 0;
            context.stalled = result.stalled;
            context.stallSeconds = stallSeconds;
            TransportFailure failure = describeCurlFailure(result.curlCode, context);
            ChatError error =
                makeError(ChatError::Code::Network, std::move(failure.message), failure.retryable);
            error.httpStatus = status;
            error.partialText = partialText();
            fail(std::move(error));
            return;
        }
        if (isSuccess(status) && events == 0 && result.bytesIn > 0 &&
            !couldBeEventStream(bodyHead)) {
            // A 200 that is not an event stream at all: a login page of a hotspot, a proxy's
            // answer, a wrong server. Trying again would only fetch the same page.
            ChatError error = makeError(
                ChatError::Code::Other,
                "the server answered, but not with an event stream (is something between this "
                "computer and the API, such as a login page or a proxy?)",
                false);
            error.httpStatus = status;
            fail(std::move(error));
            return;
        }
        // The connection ended cleanly without the end of the answer.
        if (decoder.has_value()) {
            decoder->onTransportEnd();
        }
    }
};

}  // namespace

std::vector<Header> makeAnthropicHeaders(AuthStyle auth, std::shared_ptr<const SecretString> key) {
    std::vector<Header> headers;
    headers.push_back(Header::plain("content-type", "application/json"));
    headers.push_back(Header::plain("accept", "text/event-stream"));
    headers.push_back(Header::plain("anthropic-version", std::string(kAnthropicVersion)));
    headers.push_back(Header::plain("user-agent", "Curia/" + std::string(versionString())));
    if (auth == AuthStyle::Bearer) {
        headers.push_back(Header::withSecret("authorization", "Bearer ", std::move(key)));
    } else {
        headers.push_back(Header::withSecret("x-api-key", "", std::move(key)));
    }
    return headers;
}

AnthropicProvider::AnthropicProvider(net::Endpoint endpoint, SecretString apiKey,
                                     AnthropicProviderOptions options)
    : endpoint_(std::move(endpoint)), key_(std::make_shared<const SecretString>(std::move(apiKey))),
      options_(std::move(options)), client_(options_.http) {
    // No log line, error or token may carry the key from here on.
    SecretRegistry::instance().add(*key_);
    stallSeconds_ =
        static_cast<int>(planTransfer(endpoint_.url, endpoint_, false, options_.http).stallSeconds);
}

AnthropicProvider::~AnthropicProvider() {
    closing_->store(true);
    // client_ is destroyed next (declared last): in-flight requests end with End::Shutdown, whose
    // report is muted, and the net thread is joined.
}

void AnthropicProvider::streamChat(const ChatRequest& request, StreamHandlers handlers,
                                   providers::CancelToken cancel) {
    auto state = std::make_shared<StreamState>();
    state->user = std::move(handlers);
    state->closing = closing_;
    state->log = options_.log;
    state->onNet = options_.onNetDone;
    state->stallSeconds = stallSeconds_;

    StreamHandlers inner;
    StreamState* raw = state.get();
    inner.onToken = [raw](TokenKind kind, std::string_view text) { raw->token(kind, text); };
    inner.onDone = [raw](const Usage& usage) { raw->done(usage); };
    inner.onError = [raw](const ChatError& error) { raw->fail(error); };
    inner.onStatus = [raw](StreamStatus status) {
        if (!raw->terminated && !raw->muted() && raw->user.onStatus) {
            raw->user.onStatus(status);
        }
    };
    state->decoder.emplace(std::move(inner), request.showThinking);

    // Reports a failure that happens before anything is sent, from the net thread like every
    // other report (from this thread only if there is no net thread at all).
    auto failEarly = [this, &state](ChatError error) {
        if (client_.ok()) {
            client_.post([state, error = std::move(error)] { state->fail(error); });
        } else {
            state->fail(std::move(error));
        }
    };

    if (key_->empty()) {
        failEarly(makeError(ChatError::Code::Auth, "no API key is configured", false));
        return;
    }
    if (!net::authMayAttach(endpoint_.url, endpoint_)) {
        failEarly(makeError(ChatError::Code::Other,
                            "the API key may not be sent to this address; nothing was sent",
                            false));
        return;
    }
    providers::BuiltRequest built = providers::buildAnthropicRequest(request);
    if (!built.problem.empty()) {
        failEarly(makeError(ChatError::Code::BadRequest, built.problem, false));
        return;
    }

    state->logLine(AppLog::Level::Info, "chat_request",
                   {{"body_bytes", built.body.size()},
                    {"messages", request.conversation.size()},
                    {"auth", options_.auth == AuthStyle::Bearer ? "bearer" : "x-api-key"},
                    {"test_endpoint", endpoint_.isTest}});

    Request outgoing;
    outgoing.url = endpoint_.url;
    outgoing.endpoint = endpoint_;
    outgoing.headers = makeAnthropicHeaders(options_.auth, key_);
    outgoing.body = std::move(built.body);
    outgoing.cancel = cancel;
    outgoing.callbacks.onHead = [state](const ResponseHead& head) { state->onHead(head); };
    outgoing.callbacks.onData = [state](std::string_view bytes) { return state->onData(bytes); };
    outgoing.callbacks.onComplete = [state](const Result& result) { state->onComplete(result); };
    client_.submit(std::move(outgoing));
}

}  // namespace curia::http
