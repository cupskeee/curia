// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_test_support.h"
#include "platform/env.h"
#include "providers/anthropic_stream.h"
#include "providers/sse_parser.h"

#ifndef CURIA_ANTHROPIC_FIXTURE_DIR
#error "CURIA_ANTHROPIC_FIXTURE_DIR must point at tests/fixtures/anthropic"
#endif

using curia::providers::AnthropicStreamDecoder;
using curia::providers::ChatError;
using curia::providers::SseEvent;
using curia::providers::SseParser;
using curia::providers::StreamHandlers;
using curia::providers::StreamStatus;
using curia::providers::TokenKind;
using curia::providers::Usage;
using nlohmann::json;
using Code = curia::providers::ChatError::Code;

namespace {

// --- a recording sink and the transcript it produces -----------------------------------------

struct Transcript {
    std::vector<std::pair<std::string, std::string>> tokens;  // kind name, text
    std::vector<std::string> statuses;
    int doneCalls = 0;
    int errorCalls = 0;
    Usage usage;
    ChatError error;
    std::string text;  // the decoder's text() at the end
    bool finished = false;

    [[nodiscard]] std::string textTokens() const {
        std::string out;
        for (const auto& [kind, piece] : tokens) {
            if (kind == "Text") {
                out += piece;
            }
        }
        return out;
    }

    [[nodiscard]] json toJson() const {
        json out = json::object();
        out["tokens"] = json::array();
        for (const auto& [kind, piece] : tokens) {
            out["tokens"].push_back({{"kind", kind}, {"text", piece}});
        }
        out["statuses"] = statuses;
        out["text"] = text;
        if (doneCalls == 1 && errorCalls == 0) {
            out["terminal"] = "done";
            out["usage"] = {{"input", usage.input},
                            {"output", usage.output},
                            {"cacheRead", usage.cacheRead},
                            {"cacheWrite", usage.cacheWrite},
                            {"stopReason", usage.stopReason}};
        } else if (errorCalls == 1 && doneCalls == 0) {
            out["terminal"] = "error";
            json e = {{"code", curia::providers::toString(error.code)},
                      {"message", error.message},
                      {"retryable", error.retryable},
                      {"httpStatus", error.httpStatus},
                      {"requestId", error.requestId},
                      {"partialText", error.partialText}};
            if (error.retryAfterSeconds) {
                e["retryAfterSeconds"] = *error.retryAfterSeconds;
            }
            out["error"] = e;
        } else {
            out["terminal"] = "none";
            out["doneCalls"] = doneCalls;
            out["errorCalls"] = errorCalls;
        }
        return out;
    }
};

struct Recorder {
    Transcript t;

    StreamHandlers handlers(bool withStatus = true) {
        StreamHandlers h;
        h.onToken = [this](TokenKind kind, std::string_view text) {
            t.tokens.emplace_back(kind == TokenKind::Text ? "Text" : "Thinking", std::string(text));
        };
        h.onDone = [this](const Usage& usage) {
            ++t.doneCalls;
            t.usage = usage;
        };
        h.onError = [this](const ChatError& error) {
            ++t.errorCalls;
            t.error = error;
        };
        if (withStatus) {
            h.onStatus = [this](StreamStatus status) {
                t.statuses.emplace_back(status == StreamStatus::ThinkingStarted ? "ThinkingStarted"
                                                                                : "TextStarted");
            };
        }
        return h;
    }
};

// The wire path of a response: bytes -> SseParser -> decoder, then the end of the connection.
// chunkSize 0 feeds everything in one call.
Transcript decodeStream(std::string_view bytes, std::size_t chunkSize, bool showThinking = false) {
    Recorder rec;
    AnthropicStreamDecoder decoder(rec.handlers(), showThinking);
    SseParser parser;
    const SseParser::EventHandler onEvent = [&decoder](const SseEvent& e) { decoder.onEvent(e); };
    if (chunkSize == 0) {
        parser.feed(bytes, onEvent);
    } else {
        for (std::size_t pos = 0; pos < bytes.size(); pos += chunkSize) {
            parser.feed(bytes.substr(pos, chunkSize), onEvent);
        }
    }
    parser.finish();
    decoder.onTransportEnd();
    rec.t.text = decoder.text();
    rec.t.finished = decoder.finished();
    return rec.t;
}

// --- events built in code -----------------------------------------------------------------------

SseEvent ev(const std::string& name, const std::string& data) {
    SseEvent e;
    e.event = name;
    e.data = data;
    return e;
}

SseEvent ev(const std::string& name, const char* data) {
    return ev(name, std::string(data));
}

SseEvent ev(const std::string& name, const json& data) {
    return ev(name, data.dump());
}

SseEvent startEvent(const json& usage = nullptr, const std::string& id = "msg_test") {
    json message = {{"id", id}, {"type", "message"}, {"role", "assistant"}};
    if (!usage.is_null()) {
        message["usage"] = usage;
    }
    return ev("message_start", {{"type", "message_start"}, {"message", message}});
}

SseEvent blockStart(const std::string& type) {
    return ev("content_block_start",
              {{"type", "content_block_start"}, {"index", 0}, {"content_block", {{"type", type}}}});
}

SseEvent textDelta(const std::string& text) {
    return ev("content_block_delta", {{"type", "content_block_delta"},
                                      {"index", 0},
                                      {"delta", {{"type", "text_delta"}, {"text", text}}}});
}

SseEvent thinkingDelta(const std::string& text) {
    return ev("content_block_delta", {{"type", "content_block_delta"},
                                      {"index", 0},
                                      {"delta", {{"type", "thinking_delta"}, {"thinking", text}}}});
}

SseEvent messageDelta(const std::string& stopReason, const json& usage = nullptr) {
    json data = {{"type", "message_delta"}, {"delta", {{"stop_reason", stopReason}}}};
    if (!usage.is_null()) {
        data["usage"] = usage;
    }
    return ev("message_delta", data);
}

SseEvent stopEvent() {
    return ev("message_stop", R"({"type":"message_stop"})");
}

SseEvent errorEvent(const std::string& type, const std::string& message) {
    return ev("error", {{"type", "error"}, {"error", {{"type", type}, {"message", message}}}});
}

// Feeds the events and then returns the transcript (no transport end).
Transcript run(const std::vector<SseEvent>& events, bool showThinking = false) {
    Recorder rec;
    AnthropicStreamDecoder decoder(rec.handlers(), showThinking);
    for (const SseEvent& e : events) {
        decoder.onEvent(e);
    }
    rec.t.text = decoder.text();
    rec.t.finished = decoder.finished();
    return rec.t;
}

bool isMalformed(const Transcript& t) {
    return t.errorCalls == 1 && t.doneCalls == 0 && t.error.code == Code::Other &&
           !t.error.retryable && t.error.message == "malformed event from the server";
}

// --- fixtures ---------------------------------------------------------------------------------

std::string fixtureFile(const std::string& name, const std::string& extension) {
    return std::string(CURIA_ANTHROPIC_FIXTURE_DIR) + "/" + name + extension;
}

std::vector<std::string> fixtureNames() {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(CURIA_ANTHROPIC_FIXTURE_DIR, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".sse") {
            names.push_back(it->path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string loadSse(const std::string& name) {
    bool ok = false;
    std::string bytes = curia::bridge::test::readFile(fixtureFile(name, ".sse"), ok);
    REQUIRE_MESSAGE(ok, "missing fixture");
    return bytes;
}

const std::vector<std::size_t>& chunkSizes() {
    static const std::vector<std::size_t> sizes = {1, 2, 3, 7, 64, 0};
    return sizes;
}

constexpr const char* kMockText = "Test answer from the mock server.";

}  // namespace

// ================================================================================================
// Fixtures
// ================================================================================================

TEST_CASE("anthropic fixtures: every stream decodes the same at every chunk size") {
    const std::vector<std::string> names = fixtureNames();
    REQUIRE(names.size() >= 14);
    for (const std::string& name : names) {
        CAPTURE(name);
        const std::string bytes = loadSse(name);
        for (const bool showThinking : {false, true}) {
            const Transcript whole = decodeStream(bytes, 0, showThinking);
            // Exactly one terminal callback, and the decoder agrees that it is finished.
            CHECK(whole.doneCalls + whole.errorCalls == 1);
            CHECK(whole.finished);
            // The text is the concatenation of the Text tokens.
            CHECK((whole.text == whole.textTokens()));
            for (const std::size_t size : chunkSizes()) {
                CAPTURE(size);
                const Transcript chunked = decodeStream(bytes, size, showThinking);
                if (chunked.toJson() != whole.toJson()) {
                    INFO("whole:\n"
                         << whole.toJson().dump(2) << "\nchunked:\n"
                         << chunked.toJson().dump(2));
                    FAIL("transcript depends on the chunking");
                }
            }
        }
    }
}

TEST_CASE("anthropic fixtures: golden transcripts") {
    // CURIA_UPDATE_GOLDEN=1 rewrites <name>.decoded.json (showThinking off); review the diff
    // against the spec by hand, and see the expectations in the next test cases.
    const bool update = !curia::platform::getEnv("CURIA_UPDATE_GOLDEN").empty();
    for (const std::string& name : fixtureNames()) {
        CAPTURE(name);
        const std::string actual = decodeStream(loadSse(name), 0).toJson().dump(2) + "\n";
        const std::string path = fixtureFile(name, ".decoded.json");
        if (update) {
            REQUIRE(curia::bridge::test::writeFile(path, actual));
            continue;
        }
        bool ok = false;
        const std::string golden = curia::bridge::test::readFile(path, ok);
        REQUIRE_MESSAGE(ok, "missing golden file");
        const json expected = json::parse(golden, nullptr, false);
        REQUIRE_FALSE(expected.is_discarded());
        if (expected != json::parse(actual)) {
            INFO("expected:\n" << golden << "\nactual:\n" << actual);
            FAIL("golden mismatch");
        }
    }
}

TEST_CASE("anthropic fixtures: what each stream must produce (from the spec)") {
    const auto text = [](const char* name) { return decodeStream(loadSse(name), 0); };

    SUBCASE("basic and the streams that differ only in framing give the same answer") {
        for (const char* name : {"basic", "crlf", "ping_interleaved", "unknown_event"}) {
            CAPTURE(name);
            const Transcript t = text(name);
            CHECK(t.doneCalls == 1);
            CHECK((t.text == kMockText));
            CHECK(t.tokens.size() == 3);
            CHECK((t.usage.input == 25));
            CHECK((t.usage.output == 12));
            CHECK((t.usage.cacheRead == 0));
            CHECK((t.usage.cacheWrite == 0));
            CHECK((t.usage.stopReason == "end_turn"));
            CHECK((t.statuses == std::vector<std::string>{"TextStarted"}));
        }
    }
    SUBCASE("split_every_byte keeps the multi-byte characters whole") {
        const Transcript t = text("split_every_byte");
        CHECK(t.doneCalls == 1);
        CHECK((t.text.find("caf\xC3\xA9 costs \xE2\x82\xAC"
                           "5 and \xF0\x9F\x98\x80 done.") != std::string::npos));
        for (const auto& token : t.tokens) {
            // Every token is delivered as valid UTF-8 (the JSON layer guarantees it).
            CHECK(json::parse("\"" + token.second + "\"", nullptr, false).is_string());
        }
    }
    SUBCASE("truncated: a Network error with the partial text") {
        const Transcript t = text("truncated");
        CHECK(t.errorCalls == 1);
        CHECK((t.error.code == Code::Network));
        CHECK(t.error.retryable);
        CHECK((t.error.message == "the connection ended before the answer was complete"));
        CHECK((t.error.partialText == "Test token 1.Test token 2."));
        CHECK((t.text == "Test token 1.Test token 2."));
        CHECK((t.error.requestId == "msg_mock0001"));
    }
    SUBCASE("midstream_error: Overloaded with three tokens of partial text") {
        const Transcript t = text("midstream_error");
        CHECK(t.errorCalls == 1);
        CHECK((t.error.code == Code::Overloaded));
        CHECK(t.error.retryable);
        CHECK((t.error.message == "Mock overloaded"));
        CHECK(t.error.httpStatus == 0);
        CHECK(t.tokens.size() == 3);
        CHECK((t.error.partialText == "Test token 1.Test token 2.Test token 3."));
    }
    SUBCASE("refusal: a Policy error") {
        const Transcript t = text("refusal");
        CHECK(t.errorCalls == 1);
        CHECK(t.doneCalls == 0);
        CHECK((t.error.code == Code::Policy));
        CHECK_FALSE(t.error.retryable);
        CHECK((t.error.message == "the model declined to answer"));
        CHECK((t.error.partialText == "Test refusal."));
    }
    SUBCASE("max_tokens: a normal done that says so") {
        const Transcript t = text("max_tokens");
        CHECK(t.doneCalls == 1);
        CHECK((t.usage.stopReason == "max_tokens"));
        CHECK((t.usage.output == 4096));
        CHECK((t.usage.input == 25));
        CHECK((t.text == kMockText));
    }
    SUBCASE("bad_json: malformed, and the later good events are not delivered") {
        const Transcript t = text("bad_json");
        CHECK(isMalformed(t));
        CHECK(t.tokens.empty());
        CHECK(t.error.partialText.empty());
    }
    SUBCASE("thinking_blocks: the statuses, and thinking tokens only on request") {
        const Transcript hidden = decodeStream(loadSse("thinking_blocks"), 0, false);
        CHECK((hidden.statuses == std::vector<std::string>{"ThinkingStarted", "TextStarted"}));
        CHECK(hidden.tokens.size() == 3);
        CHECK((hidden.text == kMockText));
        CHECK(hidden.doneCalls == 1);

        const Transcript shown = decodeStream(loadSse("thinking_blocks"), 0, true);
        CHECK((shown.statuses == hidden.statuses));
        REQUIRE(shown.tokens.size() == 5);
        CHECK((shown.tokens[0] ==
               std::pair<std::string, std::string>("Thinking", "Test thought one.")));
        CHECK((shown.tokens[1] ==
               std::pair<std::string, std::string>("Thinking", " Test thought two.")));
        CHECK((shown.tokens[2].first == "Text"));
        // The thinking is not part of the answer text, and the signature never shows.
        CHECK((shown.text == kMockText));
        for (const auto& token : shown.tokens) {
            CHECK(token.second.find("mock-signature") == std::string::npos);
        }
    }
    SUBCASE("usage: the same final numbers wherever they arrive") {
        for (const char* name : {"usage_start_only", "usage_delta_only", "usage_both"}) {
            CAPTURE(name);
            const Transcript t = text(name);
            CHECK(t.doneCalls == 1);
            CHECK((t.usage.input == 21));
            CHECK((t.usage.cacheWrite == 340));
            CHECK((t.usage.cacheRead == 120));
            CHECK((t.usage.output == 15));
            CHECK((t.usage.stopReason == "end_turn"));
        }
    }
}

// ================================================================================================
// One terminal, nothing after it
// ================================================================================================

TEST_CASE("anthropic decoder: events after the terminal one are ignored") {
    Recorder rec;
    AnthropicStreamDecoder decoder(rec.handlers(), true);
    decoder.onEvent(textDelta("a"));
    decoder.onEvent(stopEvent());
    CHECK(decoder.finished());
    decoder.onEvent(textDelta("b"));
    decoder.onEvent(thinkingDelta("c"));
    decoder.onEvent(blockStart("text"));
    decoder.onEvent(errorEvent("overloaded_error", "late"));
    decoder.onEvent(ev("content_block_delta", "{not json"));
    decoder.onEvent(stopEvent());
    CHECK(rec.t.doneCalls == 1);
    CHECK(rec.t.errorCalls == 0);
    CHECK(rec.t.tokens.size() == 1);
    CHECK(rec.t.statuses.empty());
    CHECK((decoder.text() == "a"));
}

TEST_CASE("anthropic decoder: exactly one terminal callback across every way to end") {
    SUBCASE("done, then transport end and http error") {
        Recorder rec;
        AnthropicStreamDecoder decoder(rec.handlers(), false);
        decoder.onEvent(stopEvent());
        decoder.onTransportEnd();
        decoder.onTransportEnd();
        decoder.onHttpError(500, "", "", "");
        CHECK(rec.t.doneCalls == 1);
        CHECK(rec.t.errorCalls == 0);
    }
    SUBCASE("error event, then more of everything") {
        Recorder rec;
        AnthropicStreamDecoder decoder(rec.handlers(), false);
        decoder.onEvent(errorEvent("api_error", "boom"));
        decoder.onEvent(stopEvent());
        decoder.onTransportEnd();
        decoder.onHttpError(429, "", "5", "");
        decoder.onEvent(errorEvent("overloaded_error", "again"));
        CHECK(rec.t.doneCalls == 0);
        CHECK(rec.t.errorCalls == 1);
        CHECK((rec.t.error.message == "boom"));
    }
    SUBCASE("transport end, then events and http error") {
        Recorder rec;
        AnthropicStreamDecoder decoder(rec.handlers(), false);
        decoder.onTransportEnd();
        decoder.onEvent(textDelta("late"));
        decoder.onEvent(stopEvent());
        decoder.onHttpError(500, "", "", "");
        CHECK(rec.t.doneCalls == 0);
        CHECK(rec.t.errorCalls == 1);
        CHECK((rec.t.error.code == Code::Network));
        CHECK(rec.t.tokens.empty());
    }
    SUBCASE("http error first") {
        Recorder rec;
        AnthropicStreamDecoder decoder(rec.handlers(), false);
        decoder.onHttpError(401, "", "", "req_x");
        decoder.onEvent(stopEvent());
        decoder.onTransportEnd();
        CHECK(rec.t.doneCalls == 0);
        CHECK(rec.t.errorCalls == 1);
        CHECK((rec.t.error.code == Code::Auth));
        CHECK(decoder.finished());
    }
    SUBCASE("malformed, then a good stop") {
        Recorder rec;
        AnthropicStreamDecoder decoder(rec.handlers(), false);
        decoder.onEvent(ev("message_start", "nope"));
        decoder.onEvent(stopEvent());
        CHECK(isMalformed(rec.t));
    }
    SUBCASE("a handler that calls back into the decoder cannot cause a second terminal") {
        Transcript t;
        AnthropicStreamDecoder* self = nullptr;
        StreamHandlers h;
        h.onDone = [&](const Usage&) {
            ++t.doneCalls;
            self->onEvent(stopEvent());
            self->onTransportEnd();
            self->onHttpError(500, "", "", "");
        };
        h.onError = [&](const ChatError&) { ++t.errorCalls; };
        AnthropicStreamDecoder decoder(std::move(h), false);
        self = &decoder;
        decoder.onEvent(stopEvent());
        CHECK(t.doneCalls == 1);
        CHECK(t.errorCalls == 0);
    }
}

TEST_CASE("anthropic decoder: nothing is required of the handlers") {
    AnthropicStreamDecoder decoder(StreamHandlers{}, true);
    decoder.onEvent(blockStart("thinking"));
    decoder.onEvent(thinkingDelta("t"));
    decoder.onEvent(textDelta("x"));
    CHECK((decoder.text() == "x"));
    decoder.onEvent(stopEvent());
    CHECK(decoder.finished());

    AnthropicStreamDecoder failing(StreamHandlers{}, false);
    failing.onTransportEnd();
    CHECK(failing.finished());
}

TEST_CASE("anthropic decoder: onStatus is optional") {
    Recorder rec;
    AnthropicStreamDecoder decoder(rec.handlers(false), false);
    decoder.onEvent(blockStart("thinking"));
    decoder.onEvent(blockStart("text"));
    decoder.onEvent(textDelta("x"));
    decoder.onEvent(stopEvent());
    CHECK(rec.t.doneCalls == 1);
    CHECK(rec.t.statuses.empty());
}

// ================================================================================================
// Transport end and HTTP errors
// ================================================================================================

TEST_CASE("anthropic decoder: the connection ending early") {
    Recorder rec;
    AnthropicStreamDecoder decoder(rec.handlers(), false);
    decoder.onEvent(startEvent(nullptr, "msg_abc"));
    decoder.onEvent(textDelta("part"));
    decoder.onEvent(textDelta("ial"));
    CHECK_FALSE(decoder.finished());
    decoder.onTransportEnd();
    CHECK(decoder.finished());
    REQUIRE(rec.t.errorCalls == 1);
    CHECK((rec.t.error.code == Code::Network));
    CHECK(rec.t.error.retryable);
    CHECK((rec.t.error.message == "the connection ended before the answer was complete"));
    CHECK((rec.t.error.partialText == "partial"));
    CHECK((rec.t.error.requestId == "msg_abc"));
    CHECK(rec.t.error.httpStatus == 0);

    Recorder empty;
    AnthropicStreamDecoder none(empty.handlers(), false);
    none.onTransportEnd();
    CHECK(empty.t.error.partialText.empty());
    CHECK(empty.t.error.requestId.empty());
}

TEST_CASE("anthropic decoder: an HTTP error instead of a stream") {
    Recorder rec;
    AnthropicStreamDecoder decoder(rec.handlers(), false);
    decoder.onHttpError(
        429, R"({"type":"error","error":{"type":"rate_limit_error","message":"Slow down"}})", "17",
        "req_test_1");
    REQUIRE(rec.t.errorCalls == 1);
    CHECK((rec.t.error.code == Code::RateLimit));
    CHECK(rec.t.error.retryable);
    CHECK((rec.t.error.message == "Slow down"));
    CHECK(rec.t.error.httpStatus == 429);
    CHECK((rec.t.error.retryAfterSeconds == std::optional<int>(17)));
    CHECK((rec.t.error.requestId == "req_test_1"));
    CHECK(rec.t.error.partialText.empty());

    // A status the table maps, and a body that is not JSON.
    Recorder other;
    AnthropicStreamDecoder second(other.handlers(), false);
    second.onHttpError(503, "upstream down", "Wed, 21 Oct 2026 07:28:00 GMT", "");
    REQUIRE(other.t.errorCalls == 1);
    CHECK((other.t.error.code == Code::Network));
    CHECK((other.t.error.message == "HTTP 503: upstream down"));
    CHECK_FALSE(other.t.error.retryAfterSeconds.has_value());
}

// ================================================================================================
// Event handling in detail
// ================================================================================================

TEST_CASE("anthropic decoder: the event name decides, ping and unknown events are ignored") {
    // Garbage data in events that are ignored cannot hurt.
    const Transcript t =
        run({startEvent(), ev("ping", "not even json"), ev("mock_future_event", "{{{{"),
             ev("content_block_stop", "garbage"), textDelta("ok"), ev("ping", ""), stopEvent()});
    CHECK(t.doneCalls == 1);
    CHECK(t.errorCalls == 0);
    CHECK((t.text == "ok"));

    // The name wins over a different "type" inside.
    const Transcript named =
        run({ev("content_block_delta",
                {{"type", "mock_other"}, {"delta", {{"type", "text_delta"}, {"text", "named"}}}}),
             stopEvent()});
    CHECK((named.text == "named"));

    // An event the decoder knows by name but whose JSON says another type: still by name.
    const Transcript wrongType = run({ev("message_stop", R"({"type":"error"})")});
    CHECK(wrongType.doneCalls == 1);
}

TEST_CASE("anthropic decoder: a stream without event names is read by the JSON type") {
    SseEvent a = textDelta("anon");
    a.event = "message";
    SseEvent stop = stopEvent();
    stop.event = "message";
    SseEvent start = startEvent(json{{"input_tokens", 3}});
    start.event = "message";
    const Transcript t = run({start, a, stop});
    CHECK(t.doneCalls == 1);
    CHECK((t.text == "anon"));
    CHECK(t.usage.input == 3);

    const SseEvent unknownType = ev("message", R"({"type":"mock_future_event"})");
    const SseEvent noType = ev("message", R"({"hello":1})");
    const SseEvent typeNotString = ev("message", R"({"type":5})");
    const SseEvent ping = ev("message", R"({"type":"ping"})");
    const Transcript ignored = run({unknownType, noType, typeNotString, ping});
    CHECK(ignored.doneCalls == 0);
    CHECK(ignored.errorCalls == 0);

    // A nameless event that is not JSON cannot be anything we understand: malformed.
    CHECK(isMalformed(run({ev("message", "[DONE]")})));
    CHECK(isMalformed(run({ev("message", "")})));
}

TEST_CASE("anthropic decoder: block starts") {
    const Transcript t =
        run({blockStart("thinking"), blockStart("text"), blockStart("tool_use"),
             blockStart("redacted_thinking"), blockStart("mock_future_block"), blockStart("")});
    CHECK((t.statuses == std::vector<std::string>{"ThinkingStarted", "TextStarted"}));
    CHECK(t.errorCalls == 0);
    CHECK(t.tokens.empty());

    // Status is reported whether or not thinking text is shown.
    CHECK((run({blockStart("thinking")}, true).statuses ==
           std::vector<std::string>{"ThinkingStarted"}));
}

TEST_CASE("anthropic decoder: deltas") {
    SUBCASE("kinds that carry nothing we show") {
        const Transcript t = run(
            {ev("content_block_delta", R"({"delta":{"type":"signature_delta","signature":"s"}})"),
             ev("content_block_delta",
                R"({"delta":{"type":"input_json_delta","partial_json":"{\"a\":"}})"),
             ev("content_block_delta", R"({"delta":{"type":"mock_future_delta","x":1}})"),
             ev("content_block_delta", R"({"delta":{"type":"citations_delta"}})")},
            true);
        CHECK(t.errorCalls == 0);
        CHECK(t.tokens.empty());
        CHECK(t.text.empty());
    }
    SUBCASE("a delta before any block start or message start is still delivered") {
        const Transcript t = run({textDelta("early")});
        CHECK((t.text == "early"));
        CHECK(t.statuses.empty());
    }
    SUBCASE("an empty text delta delivers no token") {
        const Transcript t = run({textDelta(""), textDelta("x"), textDelta("")});
        CHECK(t.tokens.size() == 1);
        CHECK((t.text == "x"));
        const Transcript thinking = run({thinkingDelta("")}, true);
        CHECK(thinking.tokens.empty());
        CHECK(thinking.errorCalls == 0);
    }
    SUBCASE("thinking tokens only when shown, and never in the text") {
        const Transcript hidden = run({thinkingDelta("hmm"), textDelta("x")}, false);
        CHECK(hidden.tokens.size() == 1);
        const Transcript shown = run({thinkingDelta("hmm"), textDelta("x")}, true);
        REQUIRE(shown.tokens.size() == 2);
        CHECK((shown.tokens[0].first == "Thinking"));
        CHECK((shown.text == "x"));
    }
    SUBCASE("a thinking delta with the wrong shape is malformed even when hidden") {
        CHECK(isMalformed(run(
            {ev("content_block_delta", R"({"delta":{"type":"thinking_delta","thinking":7}})")})));
    }
    SUBCASE("text with characters that need escaping in JSON") {
        const Transcript t =
            run({textDelta("quote \" backslash \\ newline \n tab \t nul \x01 slash / unicode "
                           "\xE2\x82\xAC")});
        CHECK((t.text == "quote \" backslash \\ newline \n tab \t nul \x01 slash / unicode "
                         "\xE2\x82\xAC"));
    }
    SUBCASE("a huge text delta") {
        const std::string big(std::size_t{900} * 1024, 'x');
        const Transcript t = run({textDelta(big), textDelta("tail")});
        CHECK(t.tokens.size() == 2);
        CHECK(t.text.size() == big.size() + 4);
        CHECK(t.errorCalls == 0);
    }
}

TEST_CASE("anthropic decoder: the answer is bounded") {
    // About 900 KiB per event, 19 events: more than the 16 MiB kept.
    Recorder rec;
    AnthropicStreamDecoder decoder(rec.handlers(), false);
    const std::string big(std::size_t{900} * 1024, 'x');
    for (int i = 0; i < 19 && !decoder.finished(); ++i) {
        decoder.onEvent(textDelta(big));
    }
    REQUIRE(rec.t.errorCalls == 1);
    CHECK((rec.t.error.code == Code::Other));
    CHECK_FALSE(rec.t.error.retryable);
    CHECK((rec.t.error.message == "the answer is larger than expected"));
    CHECK(decoder.text().size() <= AnthropicStreamDecoder::kMaxAnswerBytes);
    CHECK((rec.t.error.partialText == decoder.text()));
    CHECK((rec.t.error.partialText == rec.t.textTokens()));
    decoder.onEvent(textDelta("more"));
    CHECK(rec.t.errorCalls == 1);
}

TEST_CASE("anthropic decoder: the answer cap is exact") {
    // Half a MiB per event: a whole MiB would not fit one SSE event once wrapped in JSON.
    constexpr std::size_t kPieceBytes = std::size_t{512} * 1024;
    static_assert(AnthropicStreamDecoder::kMaxAnswerBytes % kPieceBytes == 0);
    constexpr std::size_t kPieces = AnthropicStreamDecoder::kMaxAnswerBytes / kPieceBytes;
    const std::string piece(kPieceBytes, 'x');
    SUBCASE("exactly the limit is accepted") {
        Recorder rec;
        AnthropicStreamDecoder decoder(rec.handlers(), false);
        for (std::size_t i = 0; i < kPieces; ++i) {
            decoder.onEvent(textDelta(piece));
        }
        decoder.onEvent(stopEvent());
        CHECK(decoder.text().size() == AnthropicStreamDecoder::kMaxAnswerBytes);
        CHECK(rec.t.errorCalls == 0);
        CHECK(rec.t.doneCalls == 1);
    }
    SUBCASE("one byte more is refused") {
        Recorder rec;
        AnthropicStreamDecoder decoder(rec.handlers(), false);
        for (std::size_t i = 0; i < kPieces; ++i) {
            decoder.onEvent(textDelta(piece));
        }
        decoder.onEvent(textDelta("y"));
        REQUIRE(rec.t.errorCalls == 1);
        CHECK(rec.t.doneCalls == 0);
        CHECK((rec.t.error.message == "the answer is larger than expected"));
        CHECK(decoder.text().size() == AnthropicStreamDecoder::kMaxAnswerBytes);
    }
}

// --- usage -------------------------------------------------------------------------------------

TEST_CASE("anthropic decoder: cumulative usage") {
    SUBCASE("a field present replaces the stored one, an absent field keeps it") {
        const Transcript t =
            run({startEvent(json{{"input_tokens", 10},
                                 {"cache_creation_input_tokens", 20},
                                 {"cache_read_input_tokens", 30},
                                 {"output_tokens", 1}}),
                 messageDelta("end_turn", json{{"output_tokens", 9}}), stopEvent()});
        CHECK((t.usage.input == 10));
        CHECK((t.usage.cacheWrite == 20));
        CHECK((t.usage.cacheRead == 30));
        CHECK((t.usage.output == 9));
    }
    SUBCASE("a later number can be lower; the last one wins") {
        const Transcript t = run({startEvent(json{{"output_tokens", 50}}),
                                  messageDelta("", json{{"output_tokens", 7}}), stopEvent()});
        CHECK((t.usage.output == 7));
    }
    SUBCASE("a later cache number can be lower too") {
        const Transcript t = run({startEvent(json{{"input_tokens", 40},
                                                  {"cache_creation_input_tokens", 20},
                                                  {"cache_read_input_tokens", 30}}),
                                  messageDelta("", json{{"input_tokens", 2},
                                                        {"cache_creation_input_tokens", 3},
                                                        {"cache_read_input_tokens", 5}}),
                                  stopEvent()});
        CHECK((t.usage.input == 2));
        CHECK((t.usage.cacheWrite == 3));
        CHECK((t.usage.cacheRead == 5));
    }
    SUBCASE("several message_delta events") {
        const Transcript t =
            run({messageDelta("", json{{"output_tokens", 3}}),
                 messageDelta("end_turn", json{{"output_tokens", 5}, {"input_tokens", 8}}),
                 messageDelta("end_turn"), stopEvent()});
        CHECK((t.usage.output == 5));
        CHECK((t.usage.input == 8));
        CHECK((t.usage.stopReason == "end_turn"));
    }
    SUBCASE("nothing at all") {
        const Transcript t = run({stopEvent()});
        CHECK(t.doneCalls == 1);
        CHECK((t.usage.input == 0));
        CHECK((t.usage.output == 0));
        CHECK((t.usage.cacheRead == 0));
        CHECK((t.usage.cacheWrite == 0));
        CHECK(t.usage.stopReason.empty());
    }
    SUBCASE("a stop reason that is null or missing keeps the earlier one") {
        const SseEvent nullReason =
            ev("message_delta", R"({"delta":{"stop_reason":null},"usage":{"output_tokens":4}})");
        const Transcript t = run({messageDelta("end_turn"), nullReason, stopEvent()});
        CHECK((t.usage.stopReason == "end_turn"));
        CHECK((t.usage.output == 4));
        const Transcript noDelta = run({ev("message_delta", R"({"delta":{}})"), stopEvent()});
        CHECK(noDelta.usage.stopReason.empty());
    }
    SUBCASE("an overlong stop reason is cut") {
        const Transcript t = run({messageDelta(std::string(500, 's')), stopEvent()});
        CHECK(t.usage.stopReason.size() == 64);
    }
}

TEST_CASE("anthropic decoder: usage numbers are clamped, odd values are absent") {
    const auto finalUsage = [](const std::string& usageJson) {
        return run({startEvent(json{{"input_tokens", 4},
                                    {"output_tokens", 5},
                                    {"cache_read_input_tokens", 6},
                                    {"cache_creation_input_tokens", 7}}),
                    ev("message_delta", R"({"delta":{}, "usage":)" + usageJson + "}"), stopEvent()})
            .usage;
    };
    constexpr int kMax = std::numeric_limits<int>::max();

    SUBCASE("huge integers are clamped to INT32_MAX") {
        const Usage u = finalUsage(
            R"({"input_tokens":2147483648,"output_tokens":9999999999999,)"
            R"("cache_read_input_tokens":18446744073709551615,"cache_creation_input_tokens":2147483647})");
        CHECK((u.input == kMax));
        CHECK((u.output == kMax));
        CHECK((u.cacheRead == kMax));
        CHECK((u.cacheWrite == kMax));
    }
    SUBCASE("negative numbers are absent") {
        const Usage u = finalUsage(
            R"({"input_tokens":-1,"output_tokens":-9999999999999,"cache_read_input_tokens":-0})");
        CHECK((u.input == 4));
        CHECK((u.output == 5));
        CHECK((u.cacheRead == 0));  // the literal -0 is the integer zero
        CHECK((u.cacheWrite == 7));
    }
    SUBCASE("floats, strings and other types are absent") {
        const Usage u =
            finalUsage(R"({"input_tokens":1.5,"output_tokens":"12","cache_read_input_tokens":null,)"
                       R"("cache_creation_input_tokens":[1]})");
        CHECK((u.input == 4));
        CHECK((u.output == 5));
        CHECK((u.cacheRead == 6));
        CHECK((u.cacheWrite == 7));
        const Usage v =
            finalUsage(R"({"input_tokens":1e3,"output_tokens":true,"cache_read_input_tokens":{},)"
                       R"("cache_creation_input_tokens":1.0})");
        CHECK((v.input == 4));
        CHECK((v.output == 5));
        CHECK((v.cacheRead == 6));
        CHECK((v.cacheWrite == 7));
    }
    SUBCASE("a number the JSON parser cannot hold makes the event malformed") {
        const Transcript t =
            run({ev("message_delta", R"({"delta":{},"usage":{"output_tokens":1e400}})")});
        CHECK(isMalformed(t));
    }
    SUBCASE("a usage that is not an object is ignored") {
        for (const char* odd : {"null", "5", "\"x\"", "[1,2]", "true"}) {
            CAPTURE(odd);
            const Usage u = finalUsage(odd);
            CHECK((u.input == 4));
            CHECK((u.output == 5));
        }
    }
    SUBCASE("zero is a value") {
        const Usage u = finalUsage(R"({"input_tokens":0,"output_tokens":0})");
        CHECK((u.input == 0));
        CHECK((u.output == 0));
    }
}

// --- message ids and errors in the stream -------------------------------------------------------

TEST_CASE("anthropic decoder: error events") {
    struct Row {
        const char* type;
        Code code;
        bool retryable;
    };
    const std::vector<Row> rows = {
        {"overloaded_error", Code::Overloaded, true},
        {"rate_limit_error", Code::RateLimit, true},
        {"authentication_error", Code::Auth, false},
        {"permission_error", Code::Auth, false},
        {"invalid_request_error", Code::BadRequest, false},
        {"not_found_error", Code::BadRequest, false},
        {"request_too_large", Code::BadRequest, false},
        {"api_error", Code::Other, true},
        {"timeout_error", Code::Network, true},
        {"mock_future_error", Code::Other, false},
    };
    for (const Row& row : rows) {
        CAPTURE(row.type);
        const Transcript t = run(
            {startEvent(nullptr, "msg_e1"), textDelta("so far"), errorEvent(row.type, "words")});
        REQUIRE(t.errorCalls == 1);
        CHECK((t.error.code == row.code));
        CHECK(t.error.retryable == row.retryable);
        CHECK((t.error.message == "words"));
        CHECK((t.error.partialText == "so far"));
        CHECK((t.error.requestId == "msg_e1"));
        CHECK(t.error.httpStatus == 0);
    }
    SUBCASE("the request id of the error event wins over the message id") {
        const Transcript t = run({startEvent(nullptr, "msg_e1"),
                                  ev("error", R"({"type":"error","request_id":"req_e2",)"
                                              R"("error":{"type":"api_error","message":"m"}})")});
        CHECK((t.error.requestId == "req_e2"));
    }
    SUBCASE("an error object with no type or message is still an error") {
        const Transcript t = run({ev("error", R"({"error":{}})")});
        REQUIRE(t.errorCalls == 1);
        CHECK((t.error.code == Code::Other));
        CHECK_FALSE(t.error.retryable);
        CHECK_FALSE(t.error.message.empty());
    }
    SUBCASE("a long message is cut") {
        const Transcript t = run({errorEvent("api_error", std::string(2000, 'm'))});
        CHECK(t.error.message.size() == 300);
    }
    SUBCASE("a hostile message id is made safe and short") {
        const Transcript t = run({startEvent(nullptr, std::string("a\nb") + std::string(500, 'i')),
                                  errorEvent("api_error", "m")});
        CHECK(t.error.requestId.size() == 128);
        CHECK(t.error.requestId.find('\n') == std::string::npos);
    }
}

TEST_CASE("anthropic decoder: a refusal") {
    SUBCASE("keeps the text and is a Policy error") {
        const Transcript t = run({textDelta("no"), messageDelta("refusal"), stopEvent()});
        CHECK(t.doneCalls == 0);
        REQUIRE(t.errorCalls == 1);
        CHECK((t.error.code == Code::Policy));
        CHECK_FALSE(t.error.retryable);
        CHECK((t.error.message == "the model declined to answer"));
        CHECK((t.error.partialText == "no"));
    }
    SUBCASE("a refusal without any text") {
        const Transcript t = run({messageDelta("refusal"), stopEvent()});
        CHECK((t.error.code == Code::Policy));
        CHECK(t.error.partialText.empty());
    }
    SUBCASE("only the stop reason at message_stop time counts") {
        const Transcript t = run({messageDelta("refusal"), messageDelta("end_turn"), stopEvent()});
        CHECK(t.doneCalls == 1);
        CHECK(t.errorCalls == 0);
    }
}

// --- malformed events ---------------------------------------------------------------------------

TEST_CASE("anthropic decoder: malformed events end the request") {
    const std::vector<SseEvent> bad = {
        // Not JSON, empty, or JSON that is not an object.
        ev("message_start", ""),
        ev("message_start", "{not json"),
        ev("content_block_delta", "[]"),
        ev("content_block_delta", "\"text\""),
        ev("content_block_delta", "12"),
        ev("content_block_delta", "null"),
        ev("message_stop", "garbage"),
        ev("message_stop", ""),
        ev("message_stop", "[]"),
        ev("message_stop", "5"),
        ev("message_stop", "null"),
        ev("message_stop", "\"x\""),
        ev("error", "garbage"),
        ev("message_delta", "{\"delta\":"),
        // Missing the part the type needs.
        ev("message_start", R"({"type":"message_start"})"),
        ev("message_start", R"({"message":[]})"),
        ev("message_start", R"({"message":"x"})"),
        ev("content_block_start", R"({"index":0})"),
        ev("content_block_start", R"({"content_block":"text"})"),
        ev("content_block_start", R"({"content_block":{}})"),
        ev("content_block_start", R"({"content_block":{"type":5}})"),
        ev("content_block_delta", R"({"index":0})"),
        ev("content_block_delta", R"({"delta":"x"})"),
        ev("content_block_delta", R"({"delta":{}})"),
        ev("content_block_delta", R"({"delta":{"type":3}})"),
        ev("content_block_delta", R"({"delta":{"type":"text_delta"}})"),
        ev("content_block_delta", R"({"delta":{"type":"text_delta","text":5}})"),
        ev("content_block_delta", R"({"delta":{"type":"text_delta","text":null}})"),
        ev("content_block_delta", R"({"delta":{"type":"thinking_delta"}})"),
        ev("message_delta", R"({"usage":{}})"),
        ev("message_delta", R"({"delta":[]})"),
        ev("error", R"({"type":"error"})"),
        ev("error", R"({"error":"overloaded"})"),
        ev("error", R"({"error":[]})"),
    };
    for (std::size_t i = 0; i < bad.size(); ++i) {
        CAPTURE(i);
        CAPTURE(bad[i].event);
        CAPTURE(bad[i].data);
        const Transcript t = run({textDelta("so far"), bad[i], textDelta("later"), stopEvent()});
        CHECK(isMalformed(t));
        CHECK((t.error.partialText == "so far"));
        CHECK(t.tokens.size() == 1);
    }
}

TEST_CASE("anthropic decoder: deeply nested data is malformed, without recursion") {
    SUBCASE("ten thousand openers") {
        CHECK(isMalformed(run({ev("content_block_delta", std::string(10000, '['))})));
        CHECK(isMalformed(run({ev("message_start", std::string(10000, '{'))})));
        std::string pairs;
        for (int i = 0; i < 10000; ++i) {
            pairs += "{\"a\":";
        }
        CHECK(isMalformed(run({ev("error", pairs)})));
    }
    SUBCASE("a complete, valid structure deeper than the limit") {
        const std::size_t depth = 100;
        const std::string nested = std::string(depth, '[') + std::string(depth, ']');
        const std::string data =
            R"({"delta":{"type":"text_delta","text":"x","extra":)" + nested + "}}";
        CHECK(isMalformed(run({ev("content_block_delta", data)})));
        // Within the limit it is fine, and the extra member is ignored.
        const std::string shallow = std::string(10, '[') + std::string(10, ']');
        const Transcript t =
            run({ev("content_block_delta",
                    R"({"delta":{"type":"text_delta","text":"x","extra":)" + shallow + "}}")});
        CHECK(t.errorCalls == 0);
        CHECK((t.text == "x"));
    }
    SUBCASE("deep nesting in an event that is ignored is never looked at") {
        const Transcript t = run({ev("ping", std::string(10000, '[')), textDelta("x")});
        CHECK(t.errorCalls == 0);
        CHECK((t.text == "x"));
    }
    SUBCASE("brackets inside strings do not count") {
        const Transcript t = run({textDelta(std::string(5000, '[') + std::string(5000, '{'))});
        CHECK(t.errorCalls == 0);
        CHECK(t.text.size() == 10000);
    }
}

TEST_CASE("anthropic decoder: invalid UTF-8 in token text is a malformed event") {
    for (const char* badText : {"\xFF", "caf\xC3", "\xE2\x82", "\xED\xA0\x80", "a\xC0\xAF"}) {
        const std::string data =
            std::string(R"({"delta":{"type":"text_delta","text":")") + badText + "\"}}";
        const Transcript t = run({textDelta("so far"), ev("content_block_delta", data)});
        CHECK(isMalformed(t));
        CHECK(t.tokens.size() == 1);
        CHECK((t.error.partialText == "so far"));
    }
    const std::string thinking =
        std::string(R"({"delta":{"type":"thinking_delta","thinking":"\xFF"}})");
    CHECK(isMalformed(run({ev("content_block_delta", thinking)}, true)));
}

TEST_CASE("anthropic decoder: escaped surrogates in the JSON") {
    // A valid pair is a character; a lone surrogate escape is not valid text and is refused or
    // replaced by the JSON layer, but either way the delivered text is valid UTF-8 or the event
    // is malformed.
    const Transcript pair =
        run({ev("content_block_delta", R"({"delta":{"type":"text_delta","text":"😀"}})")});
    CHECK((pair.text == "\xF0\x9F\x98\x80"));
    const Transcript lone =
        run({ev("content_block_delta", R"({"delta":{"type":"text_delta","text":"\ud83d"}})")});
    if (lone.errorCalls == 0) {
        CHECK(json::parse("\"" + lone.text + "\"", nullptr, false).is_string());
    } else {
        CHECK(isMalformed(lone));
    }
}

TEST_CASE("anthropic decoder: unicode text split across chunks") {
    // 2-, 3- and 4-byte characters in one JSON string, fed one byte at a time and in odd chunks.
    const std::string wire = "event: content_block_delta\n"
                             "data: {\"delta\":{\"type\":\"text_delta\",\"text\":\"caf\xC3\xA9 "
                             "\xE2\x82\xAC \xF0\x9F\x98\x80\"}}\n"
                             "\n"
                             "event: message_stop\n"
                             "data: {}\n"
                             "\n";
    for (const std::size_t size : chunkSizes()) {
        CAPTURE(size);
        const Transcript t = decodeStream(wire, size);
        CHECK(t.doneCalls == 1);
        REQUIRE(t.tokens.size() == 1);
        CHECK((t.tokens[0].second == "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));
    }
}

TEST_CASE("anthropic decoder: oversized data is malformed") {
    const std::string data = R"({"delta":{"type":"text_delta","text":")" +
                             std::string(SseParser::kMaxEventBytes, 'x') + "\"}}";
    CHECK(isMalformed(run({ev("content_block_delta", data)})));
}

TEST_CASE("anthropic decoder: odd but legal orders") {
    SUBCASE("a duplicated message_start overrides the usage it names") {
        const Transcript t = run({startEvent(json{{"input_tokens", 1}}),
                                  startEvent(json{{"input_tokens", 2}}), stopEvent()});
        CHECK(t.usage.input == 2);
    }
    SUBCASE("message_delta before message_start") {
        const Transcript t =
            run({messageDelta("end_turn", json{{"output_tokens", 9}}),
                 startEvent(json{{"input_tokens", 3}, {"output_tokens", 1}}), stopEvent()});
        CHECK(t.doneCalls == 1);
        CHECK(t.usage.input == 3);
        CHECK(t.usage.output == 1);  // the later message_start replaced it
    }
    SUBCASE("message_stop at once") {
        const Transcript t = run({stopEvent()});
        CHECK(t.doneCalls == 1);
        CHECK(t.text.empty());
    }
    SUBCASE("two text blocks run together") {
        const Transcript t = run(
            {blockStart("text"), textDelta("a"), blockStart("text"), textDelta("b"), stopEvent()});
        CHECK((t.text == "ab"));
        CHECK((t.statuses == std::vector<std::string>{"TextStarted", "TextStarted"}));
    }
}

TEST_CASE("anthropic decoder: a long stream of small events") {
    Recorder rec;
    AnthropicStreamDecoder decoder(rec.handlers(), false);
    decoder.onEvent(startEvent());
    std::string expected;
    for (int i = 0; i < 5000; ++i) {
        const std::string piece = "w" + std::to_string(i) + " ";
        expected += piece;
        decoder.onEvent(textDelta(piece));
    }
    decoder.onEvent(stopEvent());
    CHECK(rec.t.doneCalls == 1);
    CHECK((decoder.text() == expected));
    CHECK(rec.t.tokens.size() == 5000);
}
