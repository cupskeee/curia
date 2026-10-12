// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace curia::http {

// Reads the response header lines libcurl hands to its header callback and keeps only what Curia
// uses: the status code, the provider's request id and the Retry-After value. Nothing else is
// stored (a header can carry anything), and what is stored is cut short and made printable.
//
// Pure: no libcurl types, so it is tested without a network.
//
// A response may be preceded by interim ones (100 Continue) and, with a proxy, by the proxy's own
// answer; a new status line starts over, and only a block with a final status (200 or more) that
// ends with its empty line completes the head. Header lines that come later (trailers) are ignored.
class HeaderCapture {
public:
    // The longest request id and Retry-After value kept, in bytes.
    static constexpr std::size_t kMaxValueBytes = 128;
    // Header lines longer than this are not looked at (the value could not be kept anyway).
    static constexpr std::size_t kMaxLineBytes = 8192;

    // Feeds one raw header line, with or without its line ending. Returns true exactly once per
    // response, on the empty line that ends the header block of a final response.
    bool feed(std::string_view line);

    // 0 until a final status line has been seen.
    int status() const noexcept { return status_; }
    // True once feed() has returned true.
    bool complete() const noexcept { return complete_; }
    // "request-id" (or "x-request-id" when that is all there is), empty if none.
    const std::string& requestId() const noexcept { return requestId_; }
    // The Retry-After value as sent (the caller decides what it means), empty if none.
    const std::string& retryAfter() const noexcept { return retryAfter_; }

    void reset();

private:
    void startResponse(int status);

    int status_ = 0;
    bool complete_ = false;
    // The current block is an interim (1xx) response.
    bool interim_ = false;
    bool inBlock_ = false;
    std::string requestId_;
    std::string retryAfter_;
    bool haveRequestIdFromPrimary_ = false;
};

// The status code of a status line ("HTTP/1.1 200 OK", "HTTP/2 429"), or -1 if the line is not one.
int parseStatusLine(std::string_view line);

}  // namespace curia::http
