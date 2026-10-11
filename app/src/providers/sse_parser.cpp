// SPDX-License-Identifier: MIT
#include "providers/sse_parser.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <system_error>
#include <utility>

namespace curia::providers {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
// A line buffer that grew beyond this is released after use instead of being kept.
constexpr std::size_t kKeepLineCapacity = std::size_t{64} * 1024;

bool allAsciiDigits(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

void release(std::string& s) {
    std::string().swap(s);
}

}  // namespace

SseStatus SseParser::feed(std::string_view bytes, const EventHandler& onEvent) {
    std::size_t pos = 0;
    while (pos < bytes.size() && status_ == SseStatus::Ok && !finished_) {
        if (!bomDone_) {
            // The first bytes of the stream: a BOM is skipped, anything else is replayed as data.
            if (consumeBomByte(bytes[pos])) {
                ++pos;
            }
            continue;
        }
        if (skipLf_) {
            skipLf_ = false;
            if (bytes[pos] == '\n') {
                ++pos;
                continue;
            }
        }
        const std::size_t end = bytes.find_first_of("\r\n", pos);
        const std::size_t stop = end == std::string_view::npos ? bytes.size() : end;
        if (!appendToLine(bytes.substr(pos, stop - pos))) {
            break;
        }
        pos = stop;
        if (end == std::string_view::npos) {
            break;
        }
        skipLf_ = bytes[end] == '\r';
        ++pos;
        endLine(onEvent);
    }
    return status_;
}

SseStatus SseParser::finish() {
    finished_ = true;
    releaseAll();
    return status_;
}

// True if `c` continues the BOM and is consumed. Otherwise the stream does not start with a BOM:
// the bytes that matched so far become the start of the first line and `c` is left for the caller.
bool SseParser::consumeBomByte(char c) {
    if (c == kBom[bomMatched_]) {
        if (++bomMatched_ == kBom.size()) {
            bomDone_ = true;
            bomMatched_ = 0;
        }
        return true;
    }
    bomDone_ = true;
    const std::size_t matched = std::exchange(bomMatched_, 0);
    appendToLine(kBom.substr(0, matched));
    return false;
}

bool SseParser::appendToLine(std::string_view part) {
    // line_.size() <= maxLineBytes_ always holds, so the subtraction cannot wrap.
    if (part.size() > maxLineBytes_ - line_.size()) {
        fail(SseStatus::LineTooLong);
        return false;
    }
    line_.append(part);
    return true;
}

void SseParser::endLine(const EventHandler& onEvent) {
    const std::string_view line = line_;
    if (line.empty()) {
        dispatch(onEvent);
    } else if (line.front() != ':') {
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            applyField(line, {});
        } else {
            std::string_view value = line.substr(colon + 1);
            if (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            applyField(line.substr(0, colon), value);
        }
    }
    if (status_ != SseStatus::Ok) {
        return;
    }
    if (line_.capacity() > kKeepLineCapacity) {
        release(line_);
    } else {
        line_.clear();
    }
}

void SseParser::applyField(std::string_view field, std::string_view value) {
    if (field == "data") {
        const std::size_t separator = hasData_ ? 1 : 0;
        // data_.size() <= maxEventBytes_ always holds.
        if (separator + value.size() > maxEventBytes_ - data_.size()) {
            fail(SseStatus::EventTooLong);
            return;
        }
        if (hasData_) {
            data_ += '\n';
        }
        data_.append(value);
        hasData_ = true;
    } else if (field == "event") {
        eventType_.assign(value);
    } else if (field == "id") {
        if (value.find('\0') == std::string_view::npos) {
            id_.assign(value);
        }
    } else if (field == "retry") {
        if (allAsciiDigits(value)) {
            long long parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            // A number too large for long long is ignored like any other bad value.
            if (result.ec == std::errc()) {
                retryMs_ = parsed;
            }
        }
    }
    // Any other field name is ignored.
}

void SseParser::dispatch(const EventHandler& onEvent) {
    if (hasData_) {
        SseEvent event;
        event.event = eventType_.empty() ? std::string("message") : std::move(eventType_);
        event.data = std::move(data_);
        event.id = std::move(id_);
        event.retryMs = retryMs_;
        resetBlock();
        if (onEvent) {
            onEvent(event);
        }
        return;
    }
    resetBlock();
}

// Moved-from strings are only valid, not empty, so the block is cleared explicitly.
void SseParser::resetBlock() {
    eventType_.clear();
    data_.clear();
    hasData_ = false;
    id_.clear();
    retryMs_.reset();
}

void SseParser::releaseAll() {
    release(line_);
    release(eventType_);
    release(data_);
    release(id_);
    hasData_ = false;
    retryMs_.reset();
    bomMatched_ = 0;
}

void SseParser::fail(SseStatus status) {
    status_ = status;
    releaseAll();
}

}  // namespace curia::providers
