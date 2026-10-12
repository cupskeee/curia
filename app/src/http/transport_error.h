// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace curia::http {

// How a failed transfer is described to the player and whether trying again could help.
struct TransportFailure {
    // A plain sentence for the status line and the log; ends with "(curl N)". Never a URL, header
    // or key.
    std::string message;
    bool retryable = false;
};

// What is known about how far the transfer got; decides between messages for the same code.
struct TransportContext {
    // The TCP connection was made (some timing for the connect step exists).
    bool connected = false;
    // The response head or some body bytes arrived before the failure.
    bool receivedAnything = false;
    // The client's stall guard ended the transfer (curl code 28 then means "nothing arrived for
    // stallSeconds"; without it, code 28 is libcurl's own connect timeout).
    bool stalled = false;
    // The stall guard's time, named in the message of a stall.
    int stallSeconds = 45;
};

// Maps a libcurl result code (CURLcode as int, not CURLE_OK) to a description.
// A certificate that cannot be verified is not retryable (the same attempt fails again); a lost
// connection, a timeout, a failed DNS lookup or connect are. A "protocol" error after the
// connection was made means the other end did not speak HTTP (a captive portal or a wrong port):
// not retryable.
TransportFailure describeCurlFailure(int curlCode, const TransportContext& context);

}  // namespace curia::http
