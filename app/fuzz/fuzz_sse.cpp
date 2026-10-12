// SPDX-License-Identifier: MIT
// libFuzzer entry point for the Server-Sent Events parser (providers/sse_parser.h).
//
// The input is three header bytes followed by the stream. The header picks the limits and the
// chunking, so a seed file is a header and a plain stream (by convention the header is three
// printable characters):
//   byte 0, low 3 bits: limits {line, event}. 0 = the real ones (1 MiB each); 1 = {8, 12},
//                       2 = {64, 96}, 3 = {300, 200}, 4 = {16, 32}, so that the overflow paths are
//                       reached by inputs of a few hundred bytes; 5 to 7 = the real ones;
//   byte 0, bit 3 (value 8): showThinking of the stream decoder;
//   byte 1: the largest chunk size of the random chunking (1 + byte % 97);
//   byte 2: the seed of that chunking.
//
// Checked on every input (a failure aborts, which the fuzzer reports as a crash):
//  - nothing crashes or trips a sanitizer;
//  - the events and the final status are the same whether the stream is fed in one piece, in the
//    chunks the header picks, or one byte at a time;
//  - a small whole-buffer model of the standard written for this harness (it shares no code with
//    the parser, and is not the one in the unit tests) agrees on the events and the status;
//  - the bytes held never exceed what the limits allow, and a failed parser stays failed, delivers
//    nothing more and holds nothing;
//  - every event is well formed: a non-empty type, data within the event limit, no NUL in the id,
//    no negative retry;
//  - the events also drive an AnthropicStreamDecoder (providers/anthropic_stream.h), which must
//    not throw, call exactly one of onDone and onError and only once (the connection end that
//    follows the stream included), deliver nothing after that call, keep text() equal to the
//    concatenation of the Text tokens, report usage within [0, INT32_MAX], deliver no Thinking
//    token unless showThinking, and give the same transcript however the bytes were chunked.
//
// The same function is linked into curia_fuzz_sse_replay (fuzz_sse_replay_main.cpp), which feeds it
// files without libFuzzer so that every platform can replay the seed corpus.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "providers/anthropic_stream.h"
#include "providers/chat_types.h"
#include "providers/sse_parser.h"

namespace {

using curia::providers::AnthropicStreamDecoder;
using curia::providers::ChatError;
using curia::providers::SseEvent;
using curia::providers::SseParser;
using curia::providers::SseStatus;
using curia::providers::StreamHandlers;
using curia::providers::StreamStatus;
using curia::providers::TokenKind;
using curia::providers::Usage;

constexpr std::size_t kMaxInputBytes = 1 << 20;
constexpr std::size_t kHeaderBytes = 3;

void require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "fuzz_sse: invariant violated: %s\n", what);
        std::abort();
    }
}

struct Limits {
    std::size_t line = SseParser::kMaxLineBytes;
    std::size_t event = SseParser::kMaxEventBytes;
};

Limits limitsFor(std::uint8_t selector) {
    switch (selector & 7U) {
    case 1:
        return {8, 12};
    case 2:
        return {64, 96};
    case 3:
        return {300, 200};
    case 4:
        return {16, 32};
    default:
        return {};
    }
}

// Everything the decoder's handlers were called with, in order.
struct DecoderLog {
    std::vector<std::pair<bool, std::string>> tokens;  // {is thinking, text}
    std::vector<StreamStatus> statuses;
    int doneCalls = 0;
    int errorCalls = 0;
    Usage usage;
    ChatError error;
    std::string text;
};

bool operator==(const DecoderLog& a, const DecoderLog& b) {
    return a.tokens == b.tokens && a.statuses == b.statuses && a.doneCalls == b.doneCalls &&
           a.errorCalls == b.errorCalls && a.usage.input == b.usage.input &&
           a.usage.output == b.usage.output && a.usage.cacheRead == b.usage.cacheRead &&
           a.usage.cacheWrite == b.usage.cacheWrite && a.usage.stopReason == b.usage.stopReason &&
           a.error.code == b.error.code && a.error.message == b.error.message &&
           a.error.retryable == b.error.retryable && a.error.httpStatus == b.error.httpStatus &&
           a.error.retryAfterSeconds == b.error.retryAfterSeconds &&
           a.error.requestId == b.error.requestId && a.error.partialText == b.error.partialText &&
           a.text == b.text;
}

bool usageInRange(const Usage& usage) {
    constexpr int kMax = std::numeric_limits<int>::max();
    return usage.input >= 0 && usage.input <= kMax && usage.output >= 0 && usage.output <= kMax &&
           usage.cacheRead >= 0 && usage.cacheRead <= kMax && usage.cacheWrite >= 0 &&
           usage.cacheWrite <= kMax;
}

struct Outcome {
    std::vector<SseEvent> events;
    SseStatus status = SseStatus::Ok;
    DecoderLog decoded;
};

bool operator==(const Outcome& a, const Outcome& b) {
    return a.status == b.status && a.events == b.events && a.decoded == b.decoded;
}

// A decoder wired to a log that checks, as the calls happen, that no callback comes after the
// terminal one and that there is only one terminal.
class DecoderRig {
public:
    explicit DecoderRig(bool showThinking) : showThinking_(showThinking) {
        decoder_ = std::make_unique<AnthropicStreamDecoder>(makeHandlers(), showThinking);
    }

    void onEvent(const SseEvent& event) {
        const bool wasFinished = decoder_->finished();
        const std::size_t tokens = log_.tokens.size();
        const std::size_t statuses = log_.statuses.size();
        const int terminals = log_.doneCalls + log_.errorCalls;
        const std::size_t textSize = decoder_->text().size();
        decoder_->onEvent(event);
        if (wasFinished) {
            require(log_.tokens.size() == tokens && log_.statuses.size() == statuses &&
                        log_.doneCalls + log_.errorCalls == terminals &&
                        decoder_->text().size() == textSize,
                    "the decoder ignores events after the terminal one");
        }
        check();
    }

    // The stream is over: the connection ends, and every other way to end must now do nothing.
    DecoderLog finish() {
        decoder_->onTransportEnd();
        require(decoder_->finished(), "the connection end finishes the decoder");
        require(log_.doneCalls + log_.errorCalls == 1, "exactly one terminal callback");
        const DecoderLog before = log_;
        decoder_->onTransportEnd();
        decoder_->onHttpError(429, R"({"error":{"type":"overloaded_error"}})", "5", "req");
        decoder_->onEvent(SseEvent{"message_stop", "{}", "", std::nullopt});
        require(log_ == before, "nothing happens after the terminal callback");
        check();
        require(decoder_->text() == textTokens_, "text() is the concatenation of the Text tokens");
        log_.text = decoder_->text();
        return log_;
    }

private:
    StreamHandlers makeHandlers() {
        StreamHandlers h;
        h.onToken = [this](TokenKind kind, std::string_view text) {
            require(log_.doneCalls + log_.errorCalls == 0, "no token after the terminal callback");
            require(kind == TokenKind::Text || showThinking_, "no thinking token unless shown");
            log_.tokens.emplace_back(kind == TokenKind::Thinking, std::string(text));
            if (kind == TokenKind::Text) {
                textTokens_ += text;
            }
        };
        h.onStatus = [this](StreamStatus status) {
            require(log_.doneCalls + log_.errorCalls == 0, "no status after the terminal callback");
            log_.statuses.push_back(status);
        };
        h.onDone = [this](const Usage& usage) {
            require(log_.doneCalls + log_.errorCalls == 0, "one terminal callback only");
            require(usageInRange(usage), "usage within [0, INT32_MAX]");
            ++log_.doneCalls;
            log_.usage = usage;
        };
        h.onError = [this](const ChatError& error) {
            require(log_.doneCalls + log_.errorCalls == 0, "one terminal callback only");
            ++log_.errorCalls;
            log_.error = error;
            require(error.partialText == decoder_->text(), "partialText is the text so far");
        };
        return h;
    }

    void check() const {
        const int terminals = log_.doneCalls + log_.errorCalls;
        require(terminals <= 1, "at most one terminal callback");
        require(decoder_->finished() == (terminals == 1), "finished() agrees with the callbacks");
        // The full comparison is made once, at the end; per event the length is enough.
        require(decoder_->text().size() == textTokens_.size(), "text() grows with the Text tokens");
    }

    bool showThinking_;
    DecoderLog log_;
    std::string textTokens_;
    std::unique_ptr<AnthropicStreamDecoder> decoder_;
};

// The standard, whole buffer, one pass: no state survives between calls.
Outcome referenceModel(std::string_view in, const Limits& limits) {
    Outcome out;
    if (in.starts_with("\xEF\xBB\xBF")) {
        in.remove_prefix(3);
    }
    std::string data;
    bool haveData = false;
    std::string type;
    std::string id;
    std::optional<long long> retry;
    std::size_t at = 0;
    while (true) {
        std::size_t end = at;
        while (end < in.size() && in[end] != '\r' && in[end] != '\n') {
            ++end;
        }
        const std::string_view line = in.substr(at, end - at);
        if (line.size() > limits.line) {
            out.status = SseStatus::LineTooLong;
            return out;
        }
        if (end == in.size()) {
            return out;  // no terminator: the rest is discarded
        }
        at = end + 1;
        if (in[end] == '\r' && at < in.size() && in[at] == '\n') {
            ++at;
        }
        if (line.empty()) {
            if (haveData) {
                out.events.push_back(SseEvent{type.empty() ? "message" : type, data, id, retry});
            }
            data.clear();
            haveData = false;
            type.clear();
            id.clear();
            retry.reset();
            continue;
        }
        if (line.front() == ':') {
            continue;
        }
        const std::size_t colon = line.find(':');
        const std::string_view name = line.substr(0, colon);
        std::string_view value =
            colon == std::string_view::npos ? std::string_view() : line.substr(colon + 1);
        if (value.starts_with(' ')) {
            value.remove_prefix(1);
        }
        if (name == "data") {
            if (data.size() + (haveData ? 1U : 0U) + value.size() > limits.event) {
                out.status = SseStatus::EventTooLong;
                return out;
            }
            data += haveData ? "\n" : "";
            data += value;
            haveData = true;
        } else if (name == "event") {
            type = value;
        } else if (name == "id") {
            if (value.find('\0') == std::string_view::npos) {
                id = value;
            }
        } else if (name == "retry") {
            const bool digits =
                !value.empty() && std::all_of(value.begin(), value.end(),
                                              [](char c) { return c >= '0' && c <= '9'; });
            if (digits) {
                long long number = 0;
                bool fits = true;
                for (const char c : value) {
                    const long long digit = c - '0';
                    if (number > (std::numeric_limits<long long>::max() - digit) / 10) {
                        fits = false;
                        break;
                    }
                    number = number * 10 + digit;
                }
                if (fits) {
                    retry = number;
                }
            }
        }
    }
}

void checkEvent(const SseEvent& event, const Limits& limits) {
    require(!event.event.empty(), "event type is never empty");
    require(event.data.size() <= limits.event, "event data within the limit");
    require(event.id.find('\0') == std::string::npos, "no NUL in an id");
    require(!event.retryMs || *event.retryMs >= 0, "retry is not negative");
}

enum class Mode : std::uint8_t { Whole, Chunked, Bytewise };

// Feeds the stream the given way, checks the per-feed invariants, and returns what came out.
Outcome runParser(std::string_view stream, const Limits& limits, Mode mode, std::uint8_t maxChunk,
                  std::uint8_t seed, bool showThinking) {
    SseParser parser(limits.line, limits.event);
    Outcome out;
    DecoderRig rig(showThinking);
    const SseParser::EventHandler handler = [&out, &rig](const SseEvent& e) {
        out.events.push_back(e);
        rig.onEvent(e);
    };
    const std::size_t bound = 3 * limits.line + limits.event;

    const auto feed = [&](std::string_view piece) {
        const SseStatus before = parser.status();
        const std::size_t eventsBefore = out.events.size();
        const SseStatus status = parser.feed(piece, handler);
        require(status == parser.status(), "feed returns the status");
        require(parser.bufferedBytes() <= bound, "held bytes within the limits");
        if (before != SseStatus::Ok) {
            require(status == before, "a failure is sticky");
            require(out.events.size() == eventsBefore, "nothing is delivered after a failure");
        }
        if (status != SseStatus::Ok) {
            require(parser.bufferedBytes() == 0, "a failed parser holds nothing");
        }
    };

    if (mode == Mode::Whole) {
        feed(stream);
    } else if (mode == Mode::Bytewise) {
        for (std::size_t pos = 0; pos < stream.size(); ++pos) {
            feed(stream.substr(pos, 1));
        }
    } else {
        std::uint32_t state = 0x9E3779B9U ^ (static_cast<std::uint32_t>(seed) * 2654435761U);
        if (state == 0) {
            state = 1;  // xorshift never leaves zero
        }
        bool lastEmpty = false;
        std::size_t pos = 0;
        while (pos < stream.size()) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            // A chunk may be empty: feed() has to cope with that too.
            std::size_t want = state % (static_cast<std::size_t>(maxChunk) + 1);
            if (want == 0 && lastEmpty) {
                want = 1;  // never two empty chunks in a row, so the loop always ends
            }
            lastEmpty = want == 0;
            const std::size_t take = std::min(want, stream.size() - pos);
            feed(stream.substr(pos, take));
            pos += take;
        }
    }
    out.status = parser.finish();
    out.decoded = rig.finish();
    require(out.status == parser.status(), "finish returns the status");
    require(parser.bufferedBytes() == 0, "finish frees everything");
    for (const SseEvent& event : out.events) {
        checkEvent(event, limits);
    }
    if (out.status != SseStatus::Ok) {
        const std::size_t count = out.events.size();
        require(parser.feed("data: x\n\n", handler) == out.status, "a failure stays after finish");
        require(out.events.size() == count, "nothing after finish");
    }
    return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < kHeaderBytes || size > kMaxInputBytes) {
        return 0;
    }
    const Limits limits = limitsFor(data[0]);
    const auto maxChunk = static_cast<std::uint8_t>(1 + data[1] % 97);
    const std::uint8_t seed = data[2];
    const std::string_view stream(reinterpret_cast<const char*>(data) + kHeaderBytes,
                                  size - kHeaderBytes);

    const bool showThinking = (data[0] & 8U) != 0;
    const Outcome whole = runParser(stream, limits, Mode::Whole, maxChunk, seed, showThinking);
    require(runParser(stream, limits, Mode::Chunked, maxChunk, seed, showThinking) == whole,
            "events, status and decoder output do not depend on the chunking");
    require(runParser(stream, limits, Mode::Bytewise, maxChunk, seed, showThinking) == whole,
            "events, status and decoder output are the same one byte at a time");
    const Outcome model = referenceModel(stream, limits);
    require(model.status == whole.status && model.events == whole.events,
            "the whole-buffer model agrees");
    return 0;
}
