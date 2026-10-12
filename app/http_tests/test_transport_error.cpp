// SPDX-License-Identifier: MIT
#include <curl/curl.h>
#include <doctest/doctest.h>

#include <string>

#include "http/transport_error.h"

using curia::http::describeCurlFailure;
using curia::http::TransportContext;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

TransportContext context(bool connected, bool received, bool stalled) {
    TransportContext value;
    value.connected = connected;
    value.receivedAnything = received;
    value.stalled = stalled;
    return value;
}

int code(CURLcode value) {
    return static_cast<int>(value);
}

}  // namespace

TEST_CASE("transport-error: retryable and not retryable failures") {
    for (const CURLcode retryable :
         {CURLE_COULDNT_RESOLVE_HOST, CURLE_COULDNT_CONNECT, CURLE_OPERATION_TIMEDOUT,
          CURLE_SSL_CONNECT_ERROR, CURLE_GOT_NOTHING, CURLE_SEND_ERROR, CURLE_RECV_ERROR,
          CURLE_PARTIAL_FILE, CURLE_OUT_OF_MEMORY}) {
        CAPTURE(static_cast<int>(retryable));
        CHECK(describeCurlFailure(code(retryable), context(true, true, false)).retryable);
    }
    for (const CURLcode fatal :
         {CURLE_PEER_FAILED_VERIFICATION, CURLE_SSL_CERTPROBLEM, CURLE_SSL_CACERT_BADFILE,
          CURLE_UNSUPPORTED_PROTOCOL, CURLE_URL_MALFORMAT, CURLE_WRITE_ERROR}) {
        CAPTURE(static_cast<int>(fatal));
        CHECK_FALSE(describeCurlFailure(code(fatal), context(true, true, false)).retryable);
    }
}

TEST_CASE("transport-error: a timeout says whether it was the stall guard or the connect timeout") {
    // The client's stall guard fired: nothing arrived for the configured time, wherever it got to.
    for (const bool connected : {false, true}) {
        const auto stalled =
            describeCurlFailure(code(CURLE_OPERATION_TIMEDOUT), context(connected, false, true));
        CHECK(contains(stalled.message, "no data arrived for 45 seconds"));
        CHECK(stalled.retryable);
    }
    // libcurl's own timeout, before anything was received: connecting took too long.
    const auto connecting =
        describeCurlFailure(code(CURLE_OPERATION_TIMEDOUT), context(false, false, false));
    CHECK(contains(connecting.message, "connecting"));
    CHECK(connecting.retryable);
    TransportContext named = context(true, false, true);
    named.stallSeconds = 7;
    CHECK(contains(describeCurlFailure(28, named).message, "no data arrived for 7 seconds"));
}

TEST_CASE("transport-error: a protocol error after connecting means the server is not the API") {
    const auto notHttp =
        describeCurlFailure(code(CURLE_UNSUPPORTED_PROTOCOL), context(true, false, false));
    CHECK(contains(notHttp.message, "does not seem to be the API"));
    CHECK(notHttp.message.find("address") == std::string::npos);
    CHECK_FALSE(notHttp.retryable);
    // Before any connection the same code means the address itself was refused.
    const auto address =
        describeCurlFailure(code(CURLE_UNSUPPORTED_PROTOCOL), context(false, false, false));
    CHECK(contains(address.message, "address is not allowed"));
    CHECK_FALSE(address.retryable);
}

TEST_CASE("transport-error: every message ends with the curl code and holds nothing else") {
    const auto failure = describeCurlFailure(56, context(true, true, false));
    CHECK(failure.message == "the connection was lost (curl 56)");
    const auto unknown = describeCurlFailure(999, context(false, false, false));
    CHECK(contains(unknown.message, "(curl 999)"));
    CHECK(unknown.retryable);
    CHECK(unknown.message.find("http") == std::string::npos);
}
