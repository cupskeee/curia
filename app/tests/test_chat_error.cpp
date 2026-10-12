// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "providers/chat_error.h"

using curia::providers::ChatError;
using curia::providers::kMaxBodyExcerptBytes;
using curia::providers::kMaxServerMessageBytes;
using curia::providers::mapHttpError;
using curia::providers::mapStreamError;
using Code = curia::providers::ChatError::Code;

namespace {

std::string errorBody(const std::string& type, const std::string& message) {
    return R"({"type":"error","error":{"type":")" + type + R"(","message":")" + message + R"("}})";
}

struct StatusRow {
    int status;
    Code code;
    bool retryable;
};

}  // namespace

TEST_CASE("mapHttpError: the status table, with no usable body") {
    const std::vector<StatusRow> table = {
        {400, Code::BadRequest, false}, {401, Code::Auth, false},
        {402, Code::BadRequest, false}, {403, Code::Auth, false},
        {404, Code::BadRequest, false}, {405, Code::BadRequest, false},
        {408, Code::Network, true},     {409, Code::BadRequest, false},
        {413, Code::BadRequest, false}, {422, Code::BadRequest, false},
        {429, Code::RateLimit, true},   {499, Code::BadRequest, false},
        {500, Code::Other, true},       {501, Code::Other, true},
        {502, Code::Network, true},     {503, Code::Network, true},
        {504, Code::Network, true},     {505, Code::Other, true},
        {529, Code::Overloaded, true},  {599, Code::Other, true},
        {300, Code::Network, false},    {301, Code::Network, false},
        {302, Code::Network, false},    {307, Code::Network, false},
        {308, Code::Network, false},    {399, Code::Network, false},
        {100, Code::Other, false},      {200, Code::Other, false},
        {204, Code::Other, false},      {600, Code::Other, false},
        {0, Code::Other, false},        {-1, Code::Other, false},
    };
    for (const StatusRow& row : table) {
        CAPTURE(row.status);
        const ChatError error = mapHttpError(row.status, "", "", "");
        CHECK((error.code == row.code));
        CHECK(error.retryable == row.retryable);
        CHECK(error.httpStatus == row.status);
        CHECK_FALSE(error.message.empty());
        CHECK_FALSE(error.retryAfterSeconds.has_value());
        CHECK(error.partialText.empty());
    }
}

TEST_CASE("mapHttpError: messages for statuses with a stock meaning") {
    CHECK(
        mapHttpError(404, "", "", "").message.find("model not found: check the configured model") !=
        std::string::npos);
    CHECK(mapHttpError(307, "", "", "").message.find("unexpected redirect") != std::string::npos);
    CHECK(mapHttpError(401, "", "", "").message.find("401") != std::string::npos);
    CHECK(mapHttpError(500, "", "", "").message.find("500") != std::string::npos);
}

TEST_CASE("mapHttpError: the request id is passed through") {
    const ChatError error = mapHttpError(500, "", "", "req_mock_0001");
    CHECK((error.requestId == "req_mock_0001"));
    CHECK(mapHttpError(500, "", "", "").requestId.empty());
}

TEST_CASE("mapHttpError: a request id from a header is bounded and cleaned") {
    // Too long: cut at the bound.
    const std::string longId(std::size_t{1000}, 'r');
    const ChatError cut = mapHttpError(500, "", "", longId);
    CHECK((cut.requestId == std::string(curia::providers::kMaxRequestIdBytes, 'r')));
    // Control characters become spaces and the ends are trimmed; invalid UTF-8 becomes '?'.
    const ChatError dirty = mapHttpError(500, "", "", "\r\nreq\x01_a\xFFz\n");
    CHECK((dirty.requestId == "req _a?z"));
    // Only whitespace and control characters: nothing is left.
    CHECK(mapHttpError(500, "", "", " \r\n\t ").requestId.empty());
}

TEST_CASE("mapHttpError: the server's JSON message is used") {
    const ChatError error = mapHttpError(
        400, errorBody("invalid_request_error", "max_tokens: must be positive"), "", "");
    CHECK((error.code == Code::BadRequest));
    CHECK_FALSE(error.retryable);
    CHECK((error.message == "max_tokens: must be positive"));
}

TEST_CASE("mapHttpError: a 404 keeps the hint about the model and adds the server's words") {
    const ChatError error =
        mapHttpError(404, errorBody("not_found_error", "model: mock-model-9"), "", "");
    CHECK((error.code == Code::BadRequest));
    CHECK_FALSE(error.retryable);
    CHECK(error.message.find("model not found: check the configured model") == 0);
    CHECK(error.message.find("mock-model-9") != std::string::npos);
}

TEST_CASE("mapHttpError: the error type in the body refines the code") {
    struct Row {
        std::string type;
        int status;
        Code code;
        bool retryable;
    };
    const std::vector<Row> rows = {
        // A generic status, a specific type: the type decides.
        {"overloaded_error", 500, Code::Overloaded, true},
        {"overloaded_error", 200, Code::Overloaded, true},
        {"rate_limit_error", 400, Code::RateLimit, true},
        {"authentication_error", 400, Code::Auth, false},
        {"permission_error", 400, Code::Auth, false},
        {"invalid_request_error", 500, Code::BadRequest, false},
        {"not_found_error", 500, Code::BadRequest, false},
        {"request_too_large", 500, Code::BadRequest, false},
        {"api_error", 400, Code::Other, true},
        {"timeout_error", 400, Code::Network, true},
        // An unknown type changes nothing.
        {"mock_future_error", 529, Code::Overloaded, true},
        {"mock_future_error", 401, Code::Auth, false},
        {"", 429, Code::RateLimit, true},
    };
    for (const Row& row : rows) {
        CAPTURE(row.type);
        CAPTURE(row.status);
        const ChatError error = mapHttpError(row.status, errorBody(row.type, "words"), "", "");
        CHECK((error.code == row.code));
        CHECK(error.retryable == row.retryable);
        CHECK((error.message == "words"));
        CHECK(error.httpStatus == row.status);
    }
}

TEST_CASE("mapHttpError: Retry-After forms") {
    const auto seconds = [](std::string_view header) {
        return mapHttpError(429, "", header, "").retryAfterSeconds;
    };
    CHECK((seconds("7") == std::optional<int>(7)));
    CHECK((seconds("0") == std::optional<int>(0)));
    CHECK((seconds("00012") == std::optional<int>(12)));
    CHECK((seconds("86400") == std::optional<int>(86400)));
    CHECK((seconds(" 30 ") == std::optional<int>(30)));
    CHECK((seconds("30\r\n") == std::optional<int>(30)));
    CHECK_FALSE(seconds("86401").has_value());
    CHECK_FALSE(seconds("99999999999999999999").has_value());
    CHECK_FALSE(seconds("1234567").has_value());
    CHECK_FALSE(seconds("").has_value());
    CHECK_FALSE(seconds("-5").has_value());
    CHECK_FALSE(seconds("+5").has_value());
    CHECK_FALSE(seconds("5.5").has_value());
    CHECK_FALSE(seconds("5s").has_value());
    CHECK_FALSE(seconds("0x10").has_value());
    CHECK_FALSE(seconds("Wed, 21 Oct 2026 07:28:00 GMT").has_value());
    CHECK_FALSE(seconds("1 2").has_value());
    // The header is read whatever the status (503 and 529 send it too).
    CHECK((mapHttpError(529, "", "12", "").retryAfterSeconds == std::optional<int>(12)));
    CHECK((mapHttpError(503, "", "12", "").retryAfterSeconds == std::optional<int>(12)));
}

TEST_CASE("mapHttpError: a body that is not the expected JSON becomes an excerpt") {
    const ChatError html = mapHttpError(502, "<html>\n<body>Bad Gateway</body>\n</html>", "", "");
    CHECK((html.code == Code::Network));
    CHECK((html.message == "HTTP 502: <html> <body>Bad Gateway</body> </html>"));

    // Valid JSON of the wrong shape is a plain body too.
    CHECK((mapHttpError(500, "[1,2]", "", "").message == "HTTP 500: [1,2]"));
    CHECK((mapHttpError(500, "{\"error\":\"flat\"}", "", "").message ==
           "HTTP 500: {\"error\":\"flat\"}"));
    CHECK((mapHttpError(500, "{\"error\":{\"type\":\"api_error\"}}", "", "").message ==
           "HTTP 500: {\"error\":{\"type\":\"api_error\"}}"));
    // An error object without a message still refines the code.
    CHECK((mapHttpError(400, "{\"error\":{\"type\":\"overloaded_error\"}}", "", "").code ==
           Code::Overloaded));
    // Broken JSON.
    CHECK((mapHttpError(500, "{\"error\":", "", "").message == "HTTP 500: {\"error\":"));
    // A 404 with a plain body keeps the model hint.
    const ChatError notFound = mapHttpError(404, "page not found", "", "");
    CHECK(notFound.message.find("model not found: check the configured model") == 0);
    CHECK(notFound.message.find("page not found") != std::string::npos);
}

TEST_CASE("mapHttpError: excerpt length and UTF-8 boundary") {
    const std::string longBody(1000, 'x');
    const ChatError error = mapHttpError(500, longBody, "", "");
    CHECK((error.message == "HTTP 500: " + std::string(kMaxBodyExcerptBytes, 'x')));

    // A 3-byte character that would straddle the 200-byte bound is dropped whole.
    const std::string euro = "\xE2\x82\xAC";
    std::string body(kMaxBodyExcerptBytes - 1, 'y');
    body += euro + euro;
    const ChatError cut = mapHttpError(500, body, "", "");
    CHECK((cut.message == "HTTP 500: " + std::string(kMaxBodyExcerptBytes - 1, 'y')));

    // Exactly fitting keeps the character.
    std::string fits(kMaxBodyExcerptBytes - 3, 'y');
    fits += euro;
    CHECK((mapHttpError(500, fits, "", "").message == "HTTP 500: " + fits));
}

TEST_CASE("mapHttpError: the server message is cut at 300 bytes on a character boundary") {
    const std::string euro = "\xE2\x82\xAC";
    std::string message(kMaxServerMessageBytes - 1, 'm');
    message += euro;
    const ChatError error = mapHttpError(400, errorBody("invalid_request_error", message), "", "");
    CHECK((error.message == std::string(kMaxServerMessageBytes - 1, 'm')));

    const ChatError longer =
        mapHttpError(400, errorBody("invalid_request_error", std::string(900, 'm')), "", "");
    CHECK(longer.message.size() == kMaxServerMessageBytes);
}

TEST_CASE("mapHttpError: unprintable and invalid text is made safe") {
    // JSON escapes decode to control characters; they become spaces.
    const ChatError error = mapHttpError(
        400, R"({"error":{"type":"invalid_request_error","message":"a\nb\u0001c"}})", "", "");
    CHECK((error.message == "a b c"));
    // A body with invalid UTF-8 is not JSON; the excerpt replaces the bad bytes.
    const ChatError bad = mapHttpError(500, "oops \xFF\xFE end", "", "");
    CHECK((bad.message == "HTTP 500: oops ?? end"));
}

TEST_CASE("mapHttpError: hostile bodies") {
    // Deep nesting is refused before the parser sees it; the body becomes an excerpt.
    const ChatError deep = mapHttpError(500, std::string(10000, '['), "", "");
    CHECK((deep.code == Code::Other));
    CHECK(deep.message.size() == std::string("HTTP 500: ").size() + kMaxBodyExcerptBytes);

    // A body larger than the JSON limit is an excerpt, not parsed.
    const std::string huge =
        errorBody("overloaded_error", std::string(std::size_t{2} * 1024 * 1024, 'z'));
    const ChatError big = mapHttpError(500, huge, "", "");
    CHECK((big.code == Code::Other));
    CHECK(big.message.size() <= std::string("HTTP 500: ").size() + kMaxBodyExcerptBytes);

    // Whitespace only.
    CHECK((mapHttpError(500, " \n\t ", "", "").message.find("500") != std::string::npos));
    CHECK(mapHttpError(500, std::string("\0\0\0", 3), "", "").message.size() > 0);
}

TEST_CASE("mapStreamError: the type table") {
    struct Row {
        std::string_view type;
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
        {"something_new_error", Code::Other, false},
        {"", Code::Other, false},
        {"OVERLOADED_ERROR", Code::Other, false},
    };
    for (const Row& row : rows) {
        CAPTURE(row.type);
        const ChatError error = mapStreamError(row.type, "words");
        CHECK((error.code == row.code));
        CHECK(error.retryable == row.retryable);
        CHECK((error.message == "words"));
        CHECK(error.httpStatus == 0);
        CHECK_FALSE(error.retryAfterSeconds.has_value());
        CHECK(error.partialText.empty());
        CHECK(error.requestId.empty());
    }
}

TEST_CASE("mapStreamError: message handling") {
    // Empty message: a description that names the type, made safe.
    const ChatError none = mapStreamError("overloaded_error", "");
    CHECK(none.message.find("overloaded_error") != std::string::npos);
    CHECK((mapStreamError("", "").message == "the service reported an error"));
    CHECK(mapStreamError("bad\ntype", "").message.find("bad type") != std::string::npos);

    CHECK(mapStreamError("api_error", std::string(900, 'q')).message.size() ==
          kMaxServerMessageBytes);
    CHECK((mapStreamError("api_error", "line one\nline two").message == "line one line two"));
}
