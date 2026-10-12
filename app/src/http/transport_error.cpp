// SPDX-License-Identifier: MIT
#include "http/transport_error.h"

#include <curl/curl.h>

namespace curia::http {

TransportFailure describeCurlFailure(int curlCode, const TransportContext& context) {
    TransportFailure failure;
    failure.retryable = true;
    switch (static_cast<CURLcode>(curlCode)) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        failure.message = "could not find the server (the name lookup failed)";
        break;
    case CURLE_COULDNT_CONNECT:
        failure.message = "could not connect to the server";
        break;
    case CURLE_OPERATION_TIMEDOUT:
        failure.message =
            context.stalled || context.receivedAnything
                ? "no data arrived for " + std::to_string(context.stallSeconds) + " seconds"
                : "timed out while connecting to the server";
        break;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
        failure.message = "the server's certificate could not be verified";
        failure.retryable = false;
        break;
    case CURLE_SSL_CONNECT_ERROR:
        failure.message = "the secure connection could not be set up";
        break;
    case CURLE_GOT_NOTHING:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_PARTIAL_FILE:
        failure.message = "the connection was lost";
        break;
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT:
        failure.retryable = false;
        failure.message = context.connected || context.receivedAnything
                              ? "the server did not answer in a way Curia understands "
                                "(it does not seem to be the API)"
                              : "the request address is not allowed";
        break;
    case CURLE_WRITE_ERROR:
        failure.message = "the response could not be processed";
        failure.retryable = false;
        break;
    default:
        failure.message = "a network error occurred";
        break;
    }
    failure.message += " (curl " + std::to_string(curlCode) + ")";
    return failure;
}

}  // namespace curia::http
