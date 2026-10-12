// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/secret.h"
#include "http/tls_policy.h"
#include "net/endpoint_policy.h"
#include "providers/cancel_token.h"

// The one place in Curia that talks to the network: a libcurl multi handle driven by one thread
// (docs/architecture.md section 6). Nothing here knows about Anthropic or about chat; the
// provider builds a Request and reads the answer through Callbacks.
namespace curia::http {

// One request header. The value is `value`, followed by the bytes of `secret` when there is one
// ("Bearer " + key). The secret is read only inside the client (the one reveal() site), when the
// header line is built on the net thread, and the temporary copy is wiped after libcurl has taken
// it; it never appears in a Request that is logged, copied or compared.
struct Header {
    std::string name;
    std::string value;
    std::shared_ptr<const SecretString> secret;

    static Header plain(std::string name, std::string value);
    static Header withSecret(std::string name, std::string prefix,
                             std::shared_ptr<const SecretString> secret);
};

// The head of the final response, delivered once, before the first body bytes.
struct ResponseHead {
    int status = 0;
    // The "request-id" header, empty if none.
    std::string requestId;
    // The "retry-after" header as sent, empty if none.
    std::string retryAfter;
};

// How a request ended.
enum class End {
    // libcurl finished the transfer, successfully or with an error (see curlCode).
    Finished,
    // The request's CancelToken was cancelled; the connection is closed.
    Cancelled,
    // The client was destroyed (or is shutting down) while the request was in flight.
    Shutdown,
    // Nothing was sent: the request broke a rule (see Result::refusal).
    Refused,
};

struct Timings {
    // Milliseconds from the start of the request; -1 when that point was never reached.
    std::int64_t connectMs = -1;
    // Until the first byte of the response body.
    std::int64_t firstByteMs = -1;
    std::int64_t totalMs = 0;
};

struct Result {
    End end = End::Finished;
    // The CURLcode of the transfer as int (0 = CURLE_OK). A transfer that the consumer stopped
    // by returning false from onData ends with the write-error code (23); abortedByConsumer says
    // so.
    int curlCode = 0;
    // libcurl's own text for a failure (printable, cut, redacted); for diagnostics only.
    std::string curlMessage;
    // Why the request was refused, for End::Refused.
    std::string refusal;
    int httpStatus = 0;
    std::string requestId;
    std::string retryAfter;
    // The stall guard ended the transfer (curlCode is then 28, CURLE_OPERATION_TIMEDOUT): nothing
    // was received or sent for ClientOptions::stallTimeout. A timeout of libcurl's own (connecting
    // took longer than connectTimeout) has curlCode 28 and this false.
    bool stalled = false;
    // Body bytes delivered to onData, and request bytes libcurl reports as sent.
    std::uint64_t bytesIn = 0;
    std::uint64_t bytesOut = 0;
    Timings timings;
    bool abortedByConsumer = false;
    // The first attempt failed with a certificate-trust error and the retry with a located CA
    // bundle was made (the result is that of the retry).
    bool tlsRetried = false;
};

// All three run on the client's net thread and must not throw (an exception is caught and ends the
// transfer). They may call submit() and cancel a CancelToken. They may also drop the last owner of
// the client (see ~HttpClient), but need not expect anything else to run after that.
struct Callbacks {
    // The final response head arrived. Optional.
    std::function<void(const ResponseHead&)> onHead;
    // Raw body bytes, as they arrive (chunked framing and any compression already removed).
    // Return false to stop the transfer. Optional.
    std::function<bool(std::string_view)> onData;
    // The request ended; called exactly once for every request that submit() accepted, after the
    // connection has been released. Required.
    std::function<void(const Result&)> onComplete;
};

struct Request {
    // Must be on the endpoint's origin (same scheme, host and port) and, for a real endpoint,
    // https.
    std::string url;
    net::Endpoint endpoint;
    std::vector<Header> headers;
    // Sent as a POST body when not empty.
    std::string body;
    Callbacks callbacks;
    // Cancelling it ends the request. submit() installs the client's waker on the token (it
    // replaces an earlier one); the waker is the same for every request of one client and is left
    // in place when a request ends, so one token may be shared by several requests of the same
    // client and each cancel wakes the net thread at once. A token shared between two clients keeps
    // the waker of the client that submitted last; the other client then notices a cancel at its
    // next poll timeout (one second).
    providers::CancelToken cancel;
};

struct ClientOptions {
    // Connecting (DNS, TCP, TLS) must finish within this.
    std::chrono::milliseconds connectTimeout{10000};
    // The stall guard: a transfer in which no byte arrives (and none is sent) for this long is
    // dropped, with Result::stalled set. Measured with the client's own clock from the start of the
    // request and from every received byte, so it fires after this time plus at most a few
    // milliseconds (libcurl's low-speed limit averages over a window and fires several seconds
    // late, so it is not used). From 1 ms to one hour. Set below 45 s only by tests.
    std::chrono::milliseconds stallTimeout{45000};
    // The CA policy; the default is TlsPolicy::Auto with the system bundle located now.
    TlsPolicy tls;
};

// What the options and the request boil down to; a pure function so that the rules (HTTPS only,
// loopback http only for a test endpoint, no proxy for a test endpoint, no redirects, one origin,
// the key only to the endpoint's URL) are tested without libcurl.
//
// The Endpoint is not trusted: a test endpoint is honoured only in a build with
// CURIA_ENABLE_DEV_ENV and only for a URL of the exact loopback form makeEndpoint() builds
// (net::isLoopbackTestUrl), and a real endpoint only for Anthropic's own URL
// (net::kAnthropicMessagesUrl). A later provider for another service widens this on purpose, in
// this function, and nowhere else.
struct TransferPlan {
    // Non-empty: the request is refused for this reason and nothing is sent.
    std::string refusal;
    // The only protocol libcurl may use: "https", or "http" for a test endpoint.
    std::string protocols;
    // The transfer must not use any proxy.
    bool disableProxy = false;
    long connectTimeoutMs = 10000;
    // The stall guard in milliseconds (at least 1), and the same rounded up to whole seconds (at
    // least 1) for the message to the player.
    long long stallMs = 45000;
    long stallSeconds = 45;
};

// `carriesSecret`: some header of the request holds a secret.
TransferPlan planTransfer(std::string_view url, const net::Endpoint& endpoint, bool carriesSecret,
                          const ClientOptions& options);

class HttpClient {
public:
    explicit HttpClient(ClientOptions options = {});
    // Cancels every request in flight (each gets End::Shutdown), joins the net thread and releases
    // everything. After it returns no callback runs.
    //
    // Called on the net thread (a callback dropped the last owner of the client) it cannot join
    // itself: it marks the client as shutting down and hands the rest to the net thread, which
    // finishes what is in flight (End::Shutdown callbacks may still run, once, shortly after the
    // destructor returned) and then frees the client's state and exits. The state those callbacks
    // use must therefore be owned by the callbacks, not by the object that was just destroyed.
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // False if the net thread could not be started; submit() then completes every request at once,
    // on the calling thread, with CURLE_FAILED_INIT.
    bool ok() const noexcept;

    // Starts the request and returns at once; never throws. The request is completed through
    // callbacks.onComplete on the net thread, also when the token was already cancelled (nothing
    // is sent then) or the request is refused.
    void submit(Request request);

    // Runs `task` on the net thread, in order with the requests' starts. For a provider that has
    // to report a failure before anything is sent without calling its handlers on the caller's
    // thread. Dropped (not run) if the client is destroyed first.
    void post(std::function<void()> task);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace curia::http
