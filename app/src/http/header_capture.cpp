// SPDX-License-Identifier: MIT
#include "http/header_capture.h"

#include <cstddef>

#include "providers/provider_util.h"

namespace curia::http {

namespace {

bool isBlank(char ch) {
    return ch == ' ' || ch == '\t';
}

std::string_view stripLineEnd(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    return line;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && isBlank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isBlank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

char lower(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

int parseStatusLine(std::string_view line) {
    line = stripLineEnd(line);
    if (line.substr(0, 5) != "HTTP/") {
        return -1;
    }
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) {
        return -1;
    }
    std::string_view rest = line.substr(space + 1);
    // Exactly three digits, then the end of the line or a blank (the reason phrase).
    if (rest.size() < 3 || (rest.size() > 3 && !isBlank(rest[3]))) {
        return -1;
    }
    int value = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        if (rest[i] < '0' || rest[i] > '9') {
            return -1;
        }
        value = value * 10 + (rest[i] - '0');
    }
    if (value < 100) {
        return -1;
    }
    return value;
}

void HeaderCapture::reset() {
    *this = HeaderCapture{};
}

void HeaderCapture::startResponse(int status) {
    inBlock_ = true;
    interim_ = status < 200;
    if (!interim_) {
        status_ = status;
    }
    requestId_.clear();
    retryAfter_.clear();
    haveRequestIdFromPrimary_ = false;
}

bool HeaderCapture::feed(std::string_view rawLine) {
    if (complete_) {
        return false;  // trailers and anything after the head
    }
    const std::string_view line = stripLineEnd(rawLine);
    if (const int status = parseStatusLine(line); status > 0) {
        startResponse(status);
        return false;
    }
    if (line.empty()) {
        const bool finalHead = inBlock_ && !interim_ && status_ >= 200;
        inBlock_ = false;
        if (finalHead) {
            complete_ = true;
            return true;
        }
        return false;
    }
    if (!inBlock_ || interim_ || line.size() > kMaxLineBytes) {
        return false;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    const std::string_view name = line.substr(0, colon);
    const std::string_view value = trim(line.substr(colon + 1));
    if (equalsIgnoreCase(name, "retry-after")) {
        retryAfter_ = providers::printableExcerpt(value, kMaxValueBytes);
    } else if (equalsIgnoreCase(name, "request-id")) {
        requestId_ = providers::printableExcerpt(value, kMaxValueBytes);
        haveRequestIdFromPrimary_ = true;
    } else if (equalsIgnoreCase(name, "x-request-id") && !haveRequestIdFromPrimary_) {
        requestId_ = providers::printableExcerpt(value, kMaxValueBytes);
    }
    return false;
}

}  // namespace curia::http
