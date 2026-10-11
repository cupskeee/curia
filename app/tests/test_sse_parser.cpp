// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "providers/sse_parser.h"

using curia::providers::SseEvent;
using curia::providers::SseParser;
using curia::providers::SseStatus;

namespace {

// A string literal with embedded NUL bytes kept.
template <std::size_t N> std::string bytes(const char (&literal)[N]) {
    return std::string(literal, N - 1);
}

const std::string kBomBytes = "\xEF\xBB\xBF";

struct Run {
    std::vector<SseEvent> events;
    SseStatus status = SseStatus::Ok;
};

// Printable, injective text for an event list, so that a failed comparison shows the difference.
std::string render(const Run& run) {
    std::ostringstream out;
    const auto escape = [&out](const std::string& s) {
        for (const char c : s) {
            const auto u = static_cast<unsigned char>(c);
            if (u >= 0x20 && u < 0x7F && c != '\\' && c != '"') {
                out << c;
            } else {
                out << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<unsigned>(u) << std::dec;
            }
        }
    };
    for (const SseEvent& e : run.events) {
        out << "[event=\"";
        escape(e.event);
        out << "\" data=\"";
        escape(e.data);
        out << "\" id=\"";
        escape(e.id);
        out << "\" retry=";
        if (e.retryMs) {
            out << *e.retryMs;
        } else {
            out << "none";
        }
        out << "]\n";
    }
    out << "status=" << static_cast<int>(run.status) << "\n";
    return out.str();
}

// Feeds `stream` cut at the given positions (sorted, each in 0..size; empty pieces are fed too),
// then finish(). The status is the one finish() returns.
Run runCut(SseParser parser, std::string_view stream, const std::vector<std::size_t>& cuts) {
    Run run;
    const auto handler = [&run](const SseEvent& e) { run.events.push_back(e); };
    std::size_t from = 0;
    for (const std::size_t cut : cuts) {
        parser.feed(stream.substr(from, cut - from), handler);
        from = cut;
    }
    parser.feed(stream.substr(from), handler);
    run.status = parser.finish();
    return run;
}

Run runWhole(std::string_view stream) {
    return runCut(SseParser(), stream, {});
}

Run runChunks(SseParser parser, std::string_view stream, std::size_t size) {
    std::vector<std::size_t> cuts;
    for (std::size_t pos = size; pos < stream.size(); pos += size) {
        cuts.push_back(pos);
    }
    return runCut(std::move(parser), stream, cuts);
}

Run runChunks(std::string_view stream, std::size_t size) {
    return runChunks(SseParser(), stream, size);
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t pos = text.find(from); pos != std::string::npos;
         pos = text.find(from, pos + to.size())) {
        text.replace(pos, from.size(), to);
    }
    return text;
}

SseEvent ev(std::string data, std::string type = "message", std::string id = "",
            std::optional<long long> retry = std::nullopt) {
    SseEvent e;
    e.event = std::move(type);
    e.data = std::move(data);
    e.id = std::move(id);
    e.retryMs = retry;
    return e;
}

// An Anthropic-style stream: the shape of the events of a streamed answer (all values synthetic).
const std::string kAnthropicStream =
    R"sse(event: message_start
data: {"type":"message_start","message":{"id":"msg_mock0001","type":"message","role":"assistant","model":"mock-model-1","content":[],"stop_reason":null,"usage":{"input_tokens":25,"output_tokens":1}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: ping
data: {"type":"ping"}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":" there"}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"output_tokens":15}}

event: message_stop
data: {"type":"message_stop"}

)sse";

// Streams used for the chunk-independence tests.
std::vector<std::string> sampleStreams() {
    std::vector<std::string> out;
    out.push_back(kAnthropicStream);
    out.push_back(replaceAll(kAnthropicStream, "\n", "\r\n"));
    out.push_back(replaceAll(kAnthropicStream, "\n", "\r"));
    out.push_back(
        kBomBytes + ": comment\nid: 7\nretry: 2500\nevent: a\ndata: one\ndata\ndata: two\n\n" +
        "data: caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80\n\r\n" + "data: \xFF\xFE bad\r\r" +
        bytes("id: n\0ul\ndata: z\n\n") + "foo\nevent\n\ndata:\n\ndata: tail\n");
    out.emplace_back("data: a\r\ndata: b\r\n\r\n\n\r\r\n\ndata: c\n\n");
    return out;
}

// --- a reference implementation of the standard, written for the tests -------------------------
//
// Whole buffer, no state kept between calls, and deliberately unlike SseParser: first the text is
// cut into a list of lines, then the lines are folded into events with the standard's data buffer
// (every data line adds a trailing LF, which dispatch removes).
Run referenceParse(const std::string& stream, std::size_t maxLine, std::size_t maxEvent) {
    Run result;
    std::string text = stream;
    if (text.compare(0, 3, kBomBytes) == 0) {
        text.erase(0, 3);
    }

    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\r' || ch == '\n') {
            lines.push_back(current);
            current.clear();
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
        } else {
            current += ch;
        }
    }
    // What is left in `current` has no terminator: it is checked against the limit, never used.
    const std::size_t terminated = lines.size();
    lines.push_back(current);

    std::string dataBuffer;
    std::string typeBuffer;
    std::string idBuffer;
    std::optional<long long> retry;
    const auto reset = [&] {
        dataBuffer.clear();
        typeBuffer.clear();
        idBuffer.clear();
        retry.reset();
    };

    for (std::size_t n = 0; n < lines.size(); ++n) {
        const std::string& line = lines[n];
        if (line.size() > maxLine) {
            result.status = SseStatus::LineTooLong;
            return result;
        }
        if (n >= terminated) {
            break;
        }
        if (line.empty()) {
            if (!dataBuffer.empty()) {
                dataBuffer.pop_back();
                SseEvent e;
                e.event = typeBuffer.empty() ? "message" : typeBuffer;
                e.data = dataBuffer;
                e.id = idBuffer;
                e.retryMs = retry;
                result.events.push_back(e);
            }
            reset();
            continue;
        }
        if (line[0] == ':') {
            continue;
        }
        std::string name = line;
        std::string value;
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            name = line.substr(0, colon);
            value = line.substr(colon + 1);
            if (!value.empty() && value[0] == ' ') {
                value.erase(0, 1);
            }
        }
        if (name == "event") {
            typeBuffer = value;
        } else if (name == "data") {
            dataBuffer += value;
            dataBuffer += '\n';
            if (dataBuffer.size() - 1 > maxEvent) {
                result.status = SseStatus::EventTooLong;
                return result;
            }
        } else if (name == "id") {
            if (value.find('\0') == std::string::npos) {
                idBuffer = value;
            }
        } else if (name == "retry") {
            const bool digits =
                !value.empty() && std::all_of(value.begin(), value.end(),
                                              [](char c) { return c >= '0' && c <= '9'; });
            if (digits) {
                long long v = 0;
                bool fits = true;
                for (const char c : value) {
                    const int d = c - '0';
                    if (v > (std::numeric_limits<long long>::max() - d) / 10) {
                        fits = false;
                        break;
                    }
                    v = v * 10 + d;
                }
                if (fits) {
                    retry = v;
                }
            }
        }
    }
    return result;
}

// A fixed generator (xorshift32): the same streams on every platform and standard library.
class Rng {
public:
    explicit Rng(std::uint32_t seed) : state_(seed == 0 ? 1U : seed) {}
    std::uint32_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    std::size_t below(std::size_t n) { return next() % n; }

private:
    std::uint32_t state_;
};

// SSE-like text made of tokens that hit every rule: terminators, field names, colons, spaces,
// digits, BOMs, NULs, multi-byte and invalid UTF-8.
std::string randomStream(Rng& rng) {
    static const std::vector<std::string> tokens = {
        "data: ",
        "data:",
        "data",
        "event: ",
        "id: ",
        "retry: ",
        ":",
        " ",
        "\n",
        "\r",
        "\r\n",
        "\n\n",
        "x",
        "hello",
        "12",
        "message",
        "ping",
        "retry",
        "event",
        "id",
        ": ",
        "\xEF\xBB",
        kBomBytes,
        "\xFF",
        "\xE2\x82\xAC",
        std::string("\0", 1),
        "dat",
        "99999999999999999999",
    };
    std::string out;
    if (rng.below(4) == 0) {
        out += kBomBytes;
    }
    const std::size_t count = rng.below(70);
    for (std::size_t i = 0; i < count; ++i) {
        out += tokens[rng.below(tokens.size())];
    }
    return out;
}

std::vector<std::size_t> randomCuts(Rng& rng, std::size_t size) {
    std::vector<std::size_t> cuts;
    const std::size_t count = rng.below(8);
    for (std::size_t i = 0; i < count; ++i) {
        cuts.push_back(rng.below(size + 1));
    }
    std::sort(cuts.begin(), cuts.end());
    return cuts;
}

}  // namespace

// --- the standard's rules ------------------------------------------------------------------------

TEST_CASE("sse: a data line and a blank line make one message event") {
    const Run run = runWhole("data: hello\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0] == ev("hello"));
    CHECK(run.status == SseStatus::Ok);
}

TEST_CASE("sse: the event type applies to one event and is reset after it") {
    const Run run = runWhole("event: first\ndata: a\n\ndata: b\n\nevent: third\ndata: c\n\n");
    REQUIRE(run.events.size() == 3);
    CHECK(run.events[0] == ev("a", "first"));
    CHECK(run.events[1] == ev("b"));
    CHECK(run.events[2] == ev("c", "third"));
}

TEST_CASE("sse: an empty event field means the default type") {
    const Run run = runWhole("event:\ndata: a\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].event == "message");
}

TEST_CASE("sse: the last event field of a block wins") {
    const Run run = runWhole("event: a\nevent: b\ndata: x\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].event == "b");
}

TEST_CASE("sse: a block without data lines dispatches nothing and resets the event type") {
    const Run run = runWhole("event: lost\n\ndata: kept\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0] == ev("kept"));
}

TEST_CASE("sse: blank lines on their own dispatch nothing") {
    CHECK(runWhole("\n\n\r\n\r\r").events.empty());
}

TEST_CASE("sse: a data line with an empty value is an event with empty data") {
    for (const char* stream : {"data:\n\n", "data\n\n", "data: \n\n"}) {
        const Run run = runWhole(stream);
        REQUIRE(run.events.size() == 1);
        CHECK(run.events[0].data.empty());
    }
}

TEST_CASE("sse: data lines are joined with LF and no trailing LF") {
    const Run run = runWhole("data: a\ndata: b\ndata\ndata: c\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].data == "a\nb\n\nc");
}

TEST_CASE("sse: one optional space after the colon is removed, no more") {
    CHECK(runWhole("data:x\n\n").events[0].data == "x");
    CHECK(runWhole("data: x\n\n").events[0].data == "x");
    CHECK(runWhole("data:  x\n\n").events[0].data == " x");
    CHECK(runWhole("data:\tx\n\n").events[0].data == "\tx");
    CHECK(runWhole("data: x \n\n").events[0].data == "x ");
    // Only the first colon splits; later ones belong to the value.
    CHECK(runWhole("data: a:b: c\n\n").events[0].data == "a:b: c");
}

TEST_CASE("sse: a line without a colon is a field name with an empty value") {
    const Run run = runWhole("data\ndata\nevent\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].data == "\n");
    CHECK(run.events[0].event == "message");
}

TEST_CASE("sse: comment lines are ignored and do not dispatch") {
    const Run run = runWhole(":keep-alive\ndata: a\n: data: b\n:\ndata: c\n:x\n\n:late\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].data == "a\nc");
}

TEST_CASE("sse: field names are exact and unknown fields are ignored") {
    const Run run = runWhole("Data: a\ndata : b\n data: c\nfoo: d\nevent2: e\ndata: kept\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0] == ev("kept"));
}

TEST_CASE("sse: id is kept for its own event only, and ignored if it holds a NUL") {
    Run run = runWhole("id: 41\ndata: a\n\ndata: b\n\n");
    REQUIRE(run.events.size() == 2);
    CHECK(run.events[0].id == "41");
    CHECK(run.events[1].id.empty());

    run = runWhole(bytes("id: 5\nid: n\0ul\ndata: a\n\n"));
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].id == "5");  // the later id with the NUL did not replace it

    run = runWhole("id: 5\nid:\ndata: a\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].id.empty());  // an empty id is a valid id and replaces the earlier one
}

TEST_CASE("sse: retry is taken only when the value is ASCII digits") {
    CHECK(runWhole("retry: 1500\ndata: a\n\n").events[0].retryMs == 1500);
    CHECK(runWhole("retry:0\ndata: a\n\n").events[0].retryMs == 0);
    CHECK(runWhole("retry: 007\ndata: a\n\n").events[0].retryMs == 7);
    CHECK(runWhole("retry: 9223372036854775807\ndata: a\n\n").events[0].retryMs ==
          std::numeric_limits<long long>::max());
    for (const char* bad : {"retry: 15a\n", "retry: -5\n", "retry: +5\n", "retry: 1 5\n",
                            "retry:\n", "retry\n", "retry: 1.5\n", "retry: 9223372036854775808\n",
                            "retry: 99999999999999999999999\n", "retry: \xD9\xA1\n"}) {
        const Run run = runWhole(std::string(bad) + "data: a\n\n");
        REQUIRE(run.events.size() == 1);
        CHECK_MESSAGE(!run.events[0].retryMs, bad);
    }
    // A later valid retry replaces an earlier one; a later bad one leaves it.
    CHECK(runWhole("retry: 1\nretry: 2\ndata: a\n\n").events[0].retryMs == 2);
    CHECK(runWhole("retry: 1\nretry: x\ndata: a\n\n").events[0].retryMs == 1);
    // It belongs to one block: not carried over, and lost with a block that dispatches nothing.
    const Run run = runWhole("retry: 9\ndata: a\n\ndata: b\n\nretry: 5\n\ndata: c\n\n");
    REQUIRE(run.events.size() == 3);
    CHECK(run.events[0].retryMs == 9);
    CHECK_FALSE(run.events[1].retryMs);
    CHECK_FALSE(run.events[2].retryMs);
}

TEST_CASE("sse: a stream that ends inside an event discards it") {
    Run run = runWhole("data: a\n\ndata: b\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].data == "a");
    run = runWhole("data: a\n\ndata: b");
    CHECK(run.events.size() == 1);
    run = runWhole("event: x\ndata: a\n");
    CHECK(run.events.empty());
    run = runWhole("data: a\n\n:comment");
    CHECK(run.events.size() == 1);
    CHECK(run.status == SseStatus::Ok);
}

TEST_CASE("sse: a final CR ends a line like LF does") {
    CHECK(runWhole("data: a\r\r").events.size() == 1);
    CHECK(runWhole("data: a\r").events.empty());
}

// --- line terminators ----------------------------------------------------------------------------

TEST_CASE("sse: LF, CRLF and CR end lines, and may be mixed") {
    const Run lf = runWhole("data: a\ndata: b\n\ndata: c\n\n");
    REQUIRE(lf.events.size() == 2);
    CHECK(render(runWhole("data: a\r\ndata: b\r\n\r\ndata: c\r\n\r\n")) == render(lf));
    CHECK(render(runWhole("data: a\rdata: b\r\rdata: c\r\r")) == render(lf));
    CHECK(render(runWhole("data: a\r\ndata: b\r\rdata: c\n\r\n")) == render(lf));
    CHECK(render(runWhole("data: a\ndata: b\r\n\rdata: c\r\r")) == render(lf));
}

TEST_CASE("sse: CR LF is one line break, CR CR and LF CR are two") {
    // One break between the lines, so the data is "a\nb" in a single event.
    Run run = runWhole("data: a\r\ndata: b\r\n\r\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].data == "a\nb");
    // CR CR: the second one is an empty line, so two events.
    run = runWhole("data: a\r\rdata: b\r\r");
    CHECK(run.events.size() == 2);
    // LF CR: also an empty line.
    run = runWhole("data: a\n\rdata: b\n\r");
    CHECK(run.events.size() == 2);
}

TEST_CASE("sse: a CR at the end of one chunk and an LF at the start of the next are one break") {
    const std::string stream = "data: a\r\ndata: b\r\n\r\n";
    for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
        const Run run = runCut(SseParser(), stream, {cut});
        INFO("cut at " << cut);
        REQUIRE(run.events.size() == 1);
        CHECK(run.events[0].data == "a\nb");
    }
    // The LF that follows a CR is swallowed even when a chunk boundary and empty chunks lie
    // between.
    SseParser parser;
    std::vector<SseEvent> events;
    const auto handler = [&events](const SseEvent& e) { events.push_back(e); };
    parser.feed("data: a\r", handler);
    parser.feed("", handler);
    parser.feed("\n", handler);
    parser.feed("data: b\r\n\r\n", handler);
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "a\nb");
}

TEST_CASE("sse: only the LF directly after a CR is swallowed") {
    // CR, data, LF: the LF after "x" is a normal terminator.
    SseParser parser;
    std::vector<SseEvent> events;
    const auto handler = [&events](const SseEvent& e) { events.push_back(e); };
    parser.feed("data: a\r", handler);
    parser.feed("data: b\n\n", handler);
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "a\nb");
}

// --- the byte order mark
// ---------------------------------------------------------------------------

TEST_CASE("sse: a BOM at the start of the stream is skipped") {
    const Run run = runWhole(kBomBytes + "data: a\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0] == ev("a"));
}

TEST_CASE("sse: a BOM split across chunks is still skipped") {
    const std::string stream = kBomBytes + "data: a\n\n";
    for (std::size_t first = 0; first <= 3; ++first) {
        for (std::size_t second = first; second <= 3; ++second) {
            const Run run = runCut(SseParser(), stream, {first, second});
            INFO("cuts " << first << " and " << second);
            REQUIRE(run.events.size() == 1);
            CHECK(run.events[0].data == "a");
        }
    }
    // Empty chunks before it do not count as the start having passed.
    SseParser parser;
    std::vector<SseEvent> events;
    const auto handler = [&events](const SseEvent& e) { events.push_back(e); };
    parser.feed("", handler);
    parser.feed(kBomBytes, handler);
    parser.feed("data: a\n\n", handler);
    CHECK(events.size() == 1);
}

TEST_CASE("sse: a BOM anywhere after the first byte is part of the data") {
    // After the first byte of the stream.
    Run run = runWhole(":x\n" + kBomBytes + "data: a\n\n");
    CHECK(run.events.empty());  // the field name is BOM + "data", which is unknown
    run = runWhole("data: a\n\n" + kBomBytes + "data: b\n\n");
    CHECK(run.events.size() == 1);
    // A second BOM at the start: the first is skipped, the second stays.
    run = runWhole(kBomBytes + kBomBytes + "data: a\n\n");
    CHECK(run.events.empty());
    // In the value, the bytes arrive unchanged.
    run = runWhole("data: " + kBomBytes + "\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].data == kBomBytes);
}

TEST_CASE("sse: the start of a BOM that does not complete is data") {
    CHECK(runWhole("\xEF\xBB"
                   "data: a\n\n")
              .events.empty());  // the field name is not "data"
    const Run run = runWhole("\xEF"
                             "x: a\ndata: b\n\n");
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].data == "b");
    // The same when the bytes arrive one at a time.
    CHECK(render(runChunks("\xEF\xBB"
                           "data: a\n\n",
                           1)) == render(runWhole("\xEF\xBB"
                                                  "data: a\n\n")));
    // A stream that ends inside a BOM has no events and is not an error.
    const Run partial = runWhole("\xEF\xBB");
    CHECK(partial.events.empty());
    CHECK(partial.status == SseStatus::Ok);
}

// --- bytes
// ----------------------------------------------------------------------------------------

TEST_CASE("sse: bytes are not decoded, valid or not") {
    const std::string multibyte = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";
    const std::string invalid = "\xFF\xFE\xC0\x80 \xED\xA0\x80 \x80";
    const Run run = runWhole("data: " + multibyte + "\n\ndata: " + invalid + "\n\n");
    REQUIRE(run.events.size() == 2);
    CHECK(run.events[0].data == multibyte);
    CHECK(run.events[1].data == invalid);
}

TEST_CASE("sse: a chunk boundary inside a multi-byte sequence keeps the bytes intact") {
    const std::string euro = "\xE2\x82\xAC";
    const std::string emoji = "\xF0\x9F\x98\x80";
    const std::string stream = "data: a" + euro + "b" + emoji + "c\n\n";
    for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
        const Run run = runCut(SseParser(), stream, {cut});
        INFO("cut at " << cut);
        REQUIRE(run.events.size() == 1);
        CHECK(run.events[0].data ==
              std::string("a").append(euro).append("b").append(emoji).append("c"));
    }
}

TEST_CASE("sse: NUL and control bytes are data, except where the rules say otherwise") {
    const Run run = runWhole(bytes("data: a\0b\x01\x1F\x7F\n\n"));
    REQUIRE(run.events.size() == 1);
    CHECK(run.events[0].data == bytes("a\0b\x01\x1F\x7F"));
    const Run nameWithNul = runWhole(bytes("da\0ta: a\n\n"));
    CHECK(nameWithNul.events.empty());
}

TEST_CASE("sse: an empty handler drops the events and does not crash") {
    SseParser parser;
    CHECK(parser.feed("data: a\n\n", SseParser::EventHandler{}) == SseStatus::Ok);
    CHECK(parser.finish() == SseStatus::Ok);
}

TEST_CASE("sse: events are delivered during feed, in order") {
    SseParser parser;
    std::vector<std::string> seen;
    parser.feed("data: 1\n\ndata: 2\n\ndata: 3",
                [&seen](const SseEvent& e) { seen.push_back(e.data); });
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == "1");
    CHECK(seen[1] == "2");
}

// --- an Anthropic-style stream ------------------------------------------------------------------

TEST_CASE("sse: an Anthropic-style stream gives its events in order") {
    const Run run = runWhole(kAnthropicStream);
    CHECK(run.status == SseStatus::Ok);
    REQUIRE(run.events.size() == 8);
    const std::vector<std::string> types = {
        "message_start",       "content_block_start", "ping",          "content_block_delta",
        "content_block_delta", "content_block_stop",  "message_delta", "message_stop"};
    for (std::size_t i = 0; i < types.size(); ++i) {
        CHECK(run.events[i].event == types[i]);
        CHECK(run.events[i].id.empty());
        CHECK_FALSE(run.events[i].retryMs);
    }
    CHECK(run.events[2].data == R"({"type":"ping"})");
    CHECK(
        run.events[3].data ==
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}})");
    CHECK(run.events[7].data == R"({"type":"message_stop"})");
}

// --- chunk independence
// ---------------------------------------------------------------------------

TEST_CASE("sse: the chunk size does not change the events") {
    for (const std::string& stream : sampleStreams()) {
        const std::string whole = render(runWhole(stream));
        for (const std::size_t size : {1U, 2U, 3U, 7U, 64U}) {
            INFO("chunk size " << size);
            CHECK(render(runChunks(stream, size)) == whole);
        }
    }
}

TEST_CASE("sse: every single split point gives the events of one feed") {
    const std::string stream =
        kBomBytes + ":c\rid: 1\r\nretry: 5\nevent: t\ndata: a\xE2\x82\xAC\r\ndata\n\r\n" +
        "data: b\n\r\ndata:\r\r";
    const std::string whole = render(runWhole(stream));
    REQUIRE(runWhole(stream).events.size() == 3);
    for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
        INFO("cut at " << cut);
        CHECK(render(runCut(SseParser(), stream, {cut})) == whole);
    }
}

TEST_CASE("sse: every pair of split points gives the events of one feed") {
    const std::string stream = kBomBytes + "id: 1\r\ndata: \xC3\xA9\r\n\r\ndata: b\n\n";
    const std::string whole = render(runWhole(stream));
    for (std::size_t first = 0; first <= stream.size(); ++first) {
        for (std::size_t second = first; second <= stream.size(); ++second) {
            INFO("cuts " << first << " and " << second);
            CHECK(render(runCut(SseParser(), stream, {first, second})) == whole);
        }
    }
}

// --- limits
// -----------------------------------------------------------------------------------------

TEST_CASE("sse: the limits are 1 MiB") {
    static_assert(SseParser::kMaxLineBytes == 1048576);
    static_assert(SseParser::kMaxEventBytes == 1048576);
}

TEST_CASE("sse: a line of exactly the limit is allowed, one byte more fails") {
    const std::size_t limit = SseParser::kMaxLineBytes;
    SUBCASE("comment line of the limit, terminated by LF and by CRLF") {
        for (const char* end : {"\n", "\r\n", "\r"}) {
            const Run run = runWhole(":" + std::string(limit - 1, 'a') + end + "data: ok\n\n");
            CHECK(run.status == SseStatus::Ok);
            CHECK(run.events.size() == 1);
        }
    }
    SUBCASE("one byte more") {
        const Run run = runWhole(":" + std::string(limit, 'a') + "\ndata: lost\n\n");
        CHECK(run.status == SseStatus::LineTooLong);
        CHECK(run.events.empty());
    }
    SUBCASE("an unterminated line fails as soon as it is too long") {
        SseParser parser;
        CHECK(parser.feed(":" + std::string(limit - 1, 'a'), {}) == SseStatus::Ok);
        CHECK(parser.feed("b", {}) == SseStatus::LineTooLong);
    }
    SUBCASE("a data line of the limit fits the line limit and its data fits the event limit") {
        const Run run = runWhole("data:" + std::string(limit - 5, 'a') + "\n\n");
        CHECK(run.status == SseStatus::Ok);
        REQUIRE(run.events.size() == 1);
        CHECK(run.events[0].data.size() == limit - 5);
    }
}

TEST_CASE("sse: event data of exactly the limit is allowed, one byte more fails") {
    const std::size_t limit = SseParser::kMaxEventBytes;
    // Two lines: a + 1 (the joining LF) + b data bytes.
    const std::size_t half = limit / 2;
    SUBCASE("exactly the limit") {
        const Run run = runWhole("data: " + std::string(half, 'a') + "\ndata: " +
                                 std::string(limit - 1 - half, 'b') + "\n\ndata: next\n\n");
        CHECK(run.status == SseStatus::Ok);
        REQUIRE(run.events.size() == 2);
        CHECK(run.events[0].data.size() == limit);
    }
    SUBCASE("one byte more") {
        const Run run = runWhole("data: ok\n\ndata: " + std::string(half, 'a') + "\ndata: " +
                                 std::string(limit - half, 'b') + "\n\ndata: lost\n\n");
        CHECK(run.status == SseStatus::EventTooLong);
        REQUIRE(run.events.size() == 1);
        CHECK(run.events[0].data == "ok");
    }
}

TEST_CASE("sse: small limits, exact boundaries") {
    SUBCASE("line limit 10") {
        // "data: abcd" is exactly 10 bytes.
        for (const char* end : {"\n\n", "\r\n\r\n", "\r\r"}) {
            const Run run = runCut(SseParser(10, 100), std::string("data: abcd") + end, {});
            CHECK(run.status == SseStatus::Ok);
            CHECK(run.events.size() == 1);
        }
        CHECK(runCut(SseParser(10, 100), "data: abcde\n\n", {}).status == SseStatus::LineTooLong);
        // The limit counts the line without its terminator, also for a line that never ends.
        CHECK(runCut(SseParser(10, 100), "data: abcde", {}).status == SseStatus::LineTooLong);
        CHECK(runCut(SseParser(10, 100), "data: abcd", {}).status == SseStatus::Ok);
    }
    SUBCASE("event limit 8 across several data lines") {
        // 4 + LF + 3 = 8.
        CHECK(runCut(SseParser(64, 8), "data: 1234\ndata: 123\n\n", {}).events.size() == 1);
        // 4 + LF + 4 = 9.
        CHECK(runCut(SseParser(64, 8), "data: 1234\ndata: 1234\n\n", {}).status ==
              SseStatus::EventTooLong);
        // A single line of 8 and of 9.
        CHECK(runCut(SseParser(64, 8), "data: 12345678\n\n", {}).events.size() == 1);
        CHECK(runCut(SseParser(64, 8), "data: 123456789\n\n", {}).status ==
              SseStatus::EventTooLong);
        // Empty data lines count their LF: "" + LF + ... eight empty lines make 7 LF, nine make 8.
        CHECK(runCut(SseParser(64, 8), "data\ndata\ndata\ndata\ndata\ndata\ndata\ndata\n\n", {})
                  .events.size() == 1);
        CHECK(
            runCut(SseParser(64, 8), "data\ndata\ndata\ndata\ndata\ndata\ndata\ndata\ndata\n\n", {})
                .events.size() == 1);
        CHECK(runCut(SseParser(64, 8),
                     "data\ndata\ndata\ndata\ndata\ndata\ndata\ndata\ndata\ndata\n\n", {})
                  .status == SseStatus::EventTooLong);
    }
    SUBCASE("a limit hit while the data lines are still open fails before the blank line") {
        SseParser parser(64, 8);
        CHECK(parser.feed("data: 1234\ndata: 1234\n", {}) == SseStatus::EventTooLong);
    }
}

TEST_CASE("sse: a failure is sticky") {
    SseParser parser(10, 20);
    std::vector<SseEvent> events;
    const auto handler = [&events](const SseEvent& e) { events.push_back(e); };
    CHECK(parser.feed("data: a\n\ndata: this line is too long\n\ndata: b\n\n", handler) ==
          SseStatus::LineTooLong);
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "a");
    CHECK(parser.status() == SseStatus::LineTooLong);
    // Later input, valid or not, delivers nothing and returns the same status.
    CHECK(parser.feed("data: c\n\n", handler) == SseStatus::LineTooLong);
    CHECK(parser.feed("", handler) == SseStatus::LineTooLong);
    CHECK(parser.feed(std::string(100, 'x') + "\n", handler) == SseStatus::LineTooLong);
    CHECK(events.size() == 1);
    CHECK(parser.finish() == SseStatus::LineTooLong);
    CHECK(parser.finish() == SseStatus::LineTooLong);
    CHECK(parser.feed("data: d\n\n", handler) == SseStatus::LineTooLong);
    CHECK(events.size() == 1);
}

TEST_CASE("sse: the first failure is the one that is kept") {
    SseParser parser(64, 4);
    CHECK(parser.feed("data: 12345\n\n", {}) == SseStatus::EventTooLong);
    CHECK(parser.feed(std::string(200, 'x'), {}) == SseStatus::EventTooLong);
    CHECK(parser.finish() == SseStatus::EventTooLong);
}

TEST_CASE("sse: the event that goes over the limit is not delivered, the ones before it are") {
    SseParser parser(64, 6);
    std::vector<SseEvent> events;
    parser.feed("data: 123456\n\ndata: 1234\ndata: 12\n\n",
                [&events](const SseEvent& e) { events.push_back(e); });
    CHECK(parser.status() == SseStatus::EventTooLong);
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == "123456");
}

TEST_CASE("sse: a failure does not depend on the chunking") {
    const std::string stream = "data: ok\n\n: " + std::string(40, 'c') + "\ndata: lost\n\n";
    const Run whole = runCut(SseParser(16, 32), stream, {});
    CHECK(whole.status == SseStatus::LineTooLong);
    CHECK(whole.events.size() == 1);
    for (const std::size_t size : {1U, 2U, 3U, 7U, 64U}) {
        INFO("chunk size " << size);
        CHECK(render(runChunks(SseParser(16, 32), stream, size)) == render(whole));
    }
}

TEST_CASE("sse: memory stays within the limits however much is fed") {
    SUBCASE("a line that never ends") {
        SseParser parser;
        const std::string block(std::size_t{64} * 1024, 'a');
        std::size_t fed = 0;
        SseStatus status = SseStatus::Ok;
        while (status == SseStatus::Ok && fed < 8 * SseParser::kMaxLineBytes) {
            status = parser.feed(block, {});
            fed += block.size();
            CHECK(parser.bufferedBytes() <= SseParser::kMaxLineBytes);
        }
        CHECK(status == SseStatus::LineTooLong);
        CHECK(fed <= SseParser::kMaxLineBytes + block.size());
        // The held bytes are freed at once.
        CHECK(parser.bufferedBytes() == 0);
    }
    SUBCASE("data lines that keep adding up") {
        SseParser parser;
        const std::string line = "data: " + std::string(1000, 'a') + "\n";
        SseStatus status = SseStatus::Ok;
        std::size_t lines = 0;
        while (status == SseStatus::Ok && lines < 5000) {
            status = parser.feed(line, {});
            ++lines;
            CHECK(parser.bufferedBytes() <= SseParser::kMaxEventBytes);
        }
        CHECK(status == SseStatus::EventTooLong);
        CHECK(parser.bufferedBytes() == 0);
    }
    SUBCASE("finish frees what an unfinished event holds") {
        SseParser parser;
        parser.feed("event: x\nid: y\ndata: " + std::string(5000, 'a') + "\nmore", {});
        CHECK(parser.bufferedBytes() > 5000);
        CHECK(parser.finish() == SseStatus::Ok);
        CHECK(parser.bufferedBytes() == 0);
    }
}

TEST_CASE("sse: after finish() the bytes are ignored") {
    SseParser parser;
    std::vector<SseEvent> events;
    const auto handler = [&events](const SseEvent& e) { events.push_back(e); };
    parser.feed("data: a\n\ndata: half", handler);
    CHECK(parser.finish() == SseStatus::Ok);
    CHECK(parser.finish() == SseStatus::Ok);
    CHECK(parser.feed(" more\n\ndata: b\n\n", handler) == SseStatus::Ok);
    CHECK(events.size() == 1);
}

TEST_CASE("sse: a retry in a block that fails is not seen") {
    SseParser parser(64, 4);
    std::vector<SseEvent> events;
    parser.feed("retry: 5\ndata: 12345\n\n", [&events](const SseEvent& e) { events.push_back(e); });
    CHECK(events.empty());
}

// --- against the reference
// ------------------------------------------------------------------------

TEST_CASE("sse: the reference model agrees on hand-written cases") {
    for (const std::string& stream : sampleStreams()) {
        INFO("stream " << render(runWhole(stream)));
        CHECK(render(runWhole(stream)) ==
              render(referenceParse(stream, SseParser::kMaxLineBytes, SseParser::kMaxEventBytes)));
    }
    // And the reference itself on a few facts, so that a wrong reference cannot hide a wrong
    // parser.
    const Run ref = referenceParse("data: a\r\ndata: b\r\n\r\ndata: c", 100, 100);
    REQUIRE(ref.events.size() == 1);
    CHECK(ref.events[0].data == "a\nb");
    CHECK(referenceParse(kBomBytes + "data: x\n\n", 100, 100).events.size() == 1);
    CHECK(referenceParse("data: abcdefghijkl\n\n", 10, 100).status == SseStatus::LineTooLong);
    CHECK(referenceParse("data: abcdefghi\n\n", 100, 8).status == SseStatus::EventTooLong);
}

TEST_CASE("sse: random streams at random chunkings match the reference") {
    Rng rng(20261011U);
    for (int round = 0; round < 4000; ++round) {
        const std::string stream = randomStream(rng);
        const std::vector<std::size_t> cuts = randomCuts(rng, stream.size());
        const Run expected =
            referenceParse(stream, SseParser::kMaxLineBytes, SseParser::kMaxEventBytes);
        const Run actual = runCut(SseParser(), stream, cuts);
        INFO("round " << round);
        REQUIRE(render(actual) == render(expected));
    }
}

TEST_CASE("sse: random streams with tiny limits match the reference, failures included") {
    Rng rng(7771234U);
    int failures = 0;
    for (int round = 0; round < 6000; ++round) {
        const std::string stream = randomStream(rng);
        const std::vector<std::size_t> cuts = randomCuts(rng, stream.size());
        const std::size_t maxLine = 4 + rng.below(30);
        const std::size_t maxEvent = 1 + rng.below(24);
        const Run expected = referenceParse(stream, maxLine, maxEvent);
        const Run actual = runCut(SseParser(maxLine, maxEvent), stream, cuts);
        failures += expected.status != SseStatus::Ok ? 1 : 0;
        INFO("round " << round << " limits " << maxLine << "/" << maxEvent);
        REQUIRE(render(actual) == render(expected));
    }
    // The generator has to reach both kinds of failure often, or this test proves little.
    CHECK(failures > 500);
}
