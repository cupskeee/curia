// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>

#include "http/header_capture.h"

using curia::http::HeaderCapture;
using curia::http::parseStatusLine;

TEST_CASE("header-capture: status lines") {
    CHECK(parseStatusLine("HTTP/1.1 200 OK\r\n") == 200);
    CHECK(parseStatusLine("HTTP/1.0 404 Not Found") == 404);
    CHECK(parseStatusLine("HTTP/2 429\r\n") == 429);
    CHECK(parseStatusLine("HTTP/2 529") == 529);
    CHECK(parseStatusLine("HTTP/1.1 100 Continue\r\n") == 100);
    CHECK(parseStatusLine("HTTP/1.1 20 OK") == -1);
    CHECK(parseStatusLine("HTTP/1.1 2000 OK") == -1);
    CHECK(parseStatusLine("HTTP/1.1 2x0 OK") == -1);
    CHECK(parseStatusLine("HTTP/1.1 099 Low") == -1);
    CHECK(parseStatusLine("HTTP/1.1") == -1);
    CHECK(parseStatusLine("http/1.1 200 OK") == -1);
    CHECK(parseStatusLine("content-type: text/event-stream") == -1);
    CHECK(parseStatusLine("") == -1);
}

TEST_CASE("header-capture: status, request id and retry-after of a normal response") {
    HeaderCapture capture;
    CHECK_FALSE(capture.feed("HTTP/1.1 429 Too Many Requests\r\n"));
    CHECK_FALSE(capture.feed("Content-Type: application/json\r\n"));
    CHECK_FALSE(capture.feed("Retry-After: 7\r\n"));
    CHECK_FALSE(capture.feed("REQUEST-ID: req_mock0001\r\n"));
    CHECK_FALSE(capture.complete());
    CHECK(capture.feed("\r\n"));
    CHECK(capture.complete());
    CHECK(capture.status() == 429);
    CHECK(capture.retryAfter() == "7");
    CHECK(capture.requestId() == "req_mock0001");
}

TEST_CASE("header-capture: an interim response is skipped") {
    HeaderCapture capture;
    CHECK_FALSE(capture.feed("HTTP/1.1 100 Continue\r\n"));
    CHECK_FALSE(capture.feed("retry-after: 99\r\n"));
    CHECK_FALSE(capture.feed("\r\n"));  // ends the interim block, not the head
    CHECK(capture.status() == 0);
    CHECK_FALSE(capture.complete());
    CHECK_FALSE(capture.feed("HTTP/1.1 200 OK\r\n"));
    CHECK(capture.feed("\r\n"));
    CHECK(capture.status() == 200);
    CHECK(capture.retryAfter().empty());
}

TEST_CASE("header-capture: the head completes once and trailers are ignored") {
    HeaderCapture capture;
    capture.feed("HTTP/2 200\r\n");
    CHECK(capture.feed("\r\n"));
    CHECK_FALSE(capture.feed("request-id: late\r\n"));
    CHECK_FALSE(capture.feed("\r\n"));
    CHECK(capture.requestId().empty());
}

TEST_CASE("header-capture: x-request-id is used only when request-id is absent") {
    HeaderCapture onlyX;
    onlyX.feed("HTTP/1.1 500 Error\r\n");
    onlyX.feed("x-request-id: from-x\r\n");
    onlyX.feed("\r\n");
    CHECK(onlyX.requestId() == "from-x");

    HeaderCapture both;
    both.feed("HTTP/1.1 500 Error\r\n");
    both.feed("request-id: primary\r\n");
    both.feed("x-request-id: secondary\r\n");
    both.feed("\r\n");
    CHECK(both.requestId() == "primary");

    HeaderCapture reversed;
    reversed.feed("HTTP/1.1 500 Error\r\n");
    reversed.feed("x-request-id: secondary\r\n");
    reversed.feed("request-id: primary\r\n");
    reversed.feed("\r\n");
    CHECK(reversed.requestId() == "primary");
}

TEST_CASE("header-capture: values are trimmed, cut and made printable") {
    HeaderCapture capture;
    capture.feed("HTTP/1.1 429 X\r\n");
    capture.feed("retry-after:   12 \t\r\n");
    capture.feed("request-id: " + std::string(500, 'a') + "\r\n");
    capture.feed("\r\n");
    CHECK(capture.retryAfter() == "12");
    CHECK(capture.requestId().size() == HeaderCapture::kMaxValueBytes);

    HeaderCapture control;
    control.feed("HTTP/1.1 500 X\r\n");
    control.feed(std::string("request-id: a\x01") + "b\r\n");
    control.feed("\r\n");
    CHECK(control.requestId() == "a b");
}

TEST_CASE("header-capture: lines that are not headers, and huge lines, are ignored") {
    HeaderCapture capture;
    capture.feed("HTTP/1.1 200 OK\r\n");
    capture.feed("no colon here\r\n");
    capture.feed(": empty name\r\n");
    capture.feed("retry-after: " + std::string(HeaderCapture::kMaxLineBytes + 10, '9') + "\r\n");
    capture.feed("set-cookie: secret=1\r\n");
    capture.feed("\r\n");
    CHECK(capture.retryAfter().empty());
    CHECK(capture.requestId().empty());
    CHECK(capture.status() == 200);
}

TEST_CASE("header-capture: headers before any status line are ignored; lines without CRLF work") {
    HeaderCapture capture;
    CHECK_FALSE(capture.feed("request-id: orphan"));
    CHECK(capture.requestId().empty());
    capture.feed("HTTP/1.1 200 OK");
    capture.feed("request-id: x1");
    CHECK(capture.feed(""));
    CHECK(capture.requestId() == "x1");
}

TEST_CASE("header-capture: reset starts over") {
    HeaderCapture capture;
    capture.feed("HTTP/1.1 200 OK\r\n");
    capture.feed("request-id: a\r\n");
    capture.feed("\r\n");
    capture.reset();
    CHECK(capture.status() == 0);
    CHECK_FALSE(capture.complete());
    CHECK(capture.requestId().empty());
    capture.feed("HTTP/1.1 503 X\r\n");
    CHECK(capture.feed("\r\n"));
    CHECK(capture.status() == 503);
}
