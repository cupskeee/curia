// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace curia::providers {

// One dispatched event of a Server-Sent Events stream.
struct SseEvent {
    // The event type; "message" when the block had no event field or an empty one.
    std::string event;
    // The data lines joined with '\n' (no trailing '\n'). May be empty: "data:" alone is an event.
    std::string data;
    // The id field of this block, empty if there was none or it contained a NUL. Unlike the
    // standard's last-event-id it is not carried over to later events (no reconnection is made).
    std::string id;
    // The retry field of this block, if it was ASCII digits that fit a long long.
    std::optional<long long> retryMs;

    bool operator==(const SseEvent&) const = default;
};

enum class SseStatus {
    Ok,
    // A physical line (without its terminator) was longer than the line limit.
    LineTooLong,
    // The data of one event, joined as delivered, was longer than the event limit.
    EventTooLong,
};

// An incremental parser of the Server-Sent Events stream format, following the parsing rules of
// the WHATWG HTML standard (section "Parsing an event stream"): lines end in LF, CRLF or a bare CR;
// a UTF-8 byte order mark at the very start of the stream is skipped; a line that starts with ':'
// is a comment; "field:value" drops one space after the colon; a line without a colon is a field
// with an empty value; the fields event, data, id and retry are used, others are ignored; an empty
// line dispatches the event.
//
// Byte oriented: the bytes are never decoded, so text may be split inside a UTF-8 sequence and
// invalid UTF-8 passes through unchanged. Which bytes arrive in which feed() call does not change
// the events, nor the status.
//
// Deviations from the standard, none of which matters without reconnection (Anthropic's stream
// has none): the id and the retry value belong to one dispatched block only (see SseEvent), so
// they are not carried over to later events, and an id or retry in a block with no data is
// dropped; a retry field with no digits is ignored; the line limit also covers comment lines. A
// stream that ends inside an event is discarded, as the standard says, and finish() reports that
// nothing more will come.
//
// Memory is bounded: at most one line (kMaxLineBytes), the data of one event (kMaxEventBytes) and
// the event type and id, each shorter than a line. Over a limit the parser fails for good: the
// status says which limit, the held bytes are freed, and no further event is delivered.
//
// Not thread-safe. The event handler must not call back into the parser.
class SseParser {
public:
    static constexpr std::size_t kMaxLineBytes = 1024 * 1024;
    static constexpr std::size_t kMaxEventBytes = 1024 * 1024;

    using EventHandler = std::function<void(const SseEvent&)>;

    SseParser() = default;
    // Other limits, for tests and the fuzz target.
    SseParser(std::size_t maxLineBytes, std::size_t maxEventBytes)
        : maxLineBytes_(maxLineBytes), maxEventBytes_(maxEventBytes) {}

    // Consumes `bytes`, calling `onEvent` for each event they complete, in order, before returning.
    // Returns the status after the last byte: Ok, or the sticky failure (then it returns at once on
    // every later call and delivers nothing). An empty handler drops the events.
    SseStatus feed(std::string_view bytes, const EventHandler& onEvent);

    // The stream ended. An event with no blank line after it is discarded, not dispatched, and so
    // is a line with no terminator. Frees what was held and returns the sticky status; later feed()
    // calls ignore their bytes. Safe to call more than once.
    SseStatus finish();

    SseStatus status() const noexcept { return status_; }

    // Bytes currently held: the partial line, the data so far and the event type and id.
    std::size_t bufferedBytes() const noexcept {
        return line_.size() + data_.size() + eventType_.size() + id_.size();
    }

private:
    bool consumeBomByte(char c);
    bool appendToLine(std::string_view part);
    void endLine(const EventHandler& onEvent);
    void applyField(std::string_view field, std::string_view value);
    void dispatch(const EventHandler& onEvent);
    void resetBlock();
    void releaseAll();
    void fail(SseStatus status);

    std::size_t maxLineBytes_ = kMaxLineBytes;
    std::size_t maxEventBytes_ = kMaxEventBytes;
    SseStatus status_ = SseStatus::Ok;
    bool finished_ = false;

    // How many bytes of the BOM matched so far; bomDone_ once the start of the stream is decided.
    std::size_t bomMatched_ = 0;
    bool bomDone_ = false;
    // The last byte fed was a CR: a LF that comes next belongs to it.
    bool skipLf_ = false;

    std::string line_;

    // The event block under construction.
    std::string eventType_;
    std::string data_;
    bool hasData_ = false;
    std::string id_;
    std::optional<long long> retryMs_;
};

}  // namespace curia::providers
