// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "core/app_log.h"
#include "core/secret.h"
#include "http/http_client.h"
#include "net/endpoint_policy.h"
#include "providers/provider.h"

namespace curia::http {

// The API version header value. The Messages API has had one version since its release, and the
// request shape Curia builds (providers/anthropic_request.h) is the one documented for it
// (research-notes C11).
inline constexpr std::string_view kAnthropicVersion = "2023-06-01";

// Which header carries the key (research-notes C11): Bearer is the documented primary; x-api-key
// is the legacy form, still accepted, kept as a fallback that the owner's preflight can select.
enum class AuthStyle { Bearer, XApiKey };

// What the transport reports about one request, once, when the connection has ended (for an
// answer that completes this comes just AFTER onDone: the server closes the connection after the
// last event). Numbers only. Used by the connection check (`curia --check-api`).
struct NetSummary {
    // The HTTP status of the final response; 0 when none arrived.
    int httpStatus = 0;
    // The first attempt failed with a certificate-trust error and the retry with a located CA
    // bundle was made.
    bool tlsRetried = false;
    // Milliseconds from the start of the request; -1 when the point was never reached.
    std::int64_t connectMs = -1;
    std::int64_t firstByteMs = -1;
    std::int64_t totalMs = 0;
    // The CURLcode of the transfer as int (0 = ok).
    int curlCode = 0;
};

struct AnthropicProviderOptions {
    AuthStyle auth = AuthStyle::Bearer;
    ClientOptions http;
    // Called on the net thread, once per request, with the numbers above. Optional; must not
    // throw.
    std::function<void(const NetSummary&)> onNetDone;
    // Where numbers about each request go (sizes, status, timings, usage); never text. Not owned,
    // may be null; must outlive the provider.
    AppLog* log = nullptr;
};

// The headers of a Messages request: content-type, accept (event stream), anthropic-version, a
// user agent, and the key in the header `auth` names. The key is carried as a secret and is read
// only by the HTTP client.
std::vector<Header> makeAnthropicHeaders(AuthStyle auth, std::shared_ptr<const SecretString> key);

// The Anthropic Messages streaming provider: builds the body (providers/anthropic_request.h),
// sends it through an HttpClient, and turns the answer into the StreamHandlers calls with the SSE
// parser and the stream decoder. Only the endpoint's own URL ever receives the key; a redirect is
// an error; errors are redacted before they leave.
//
// Handlers run on the client's net thread. Exactly one of onDone and onError is called per
// request, except that after the destructor has started no handler is called at all (the owner is
// going away, and what its handlers point to may not exist any more). The destructor cancels what
// is in flight and joins the net thread. A handler may drop the last owner of the provider: the
// destructor then cannot join (it is on that thread) and the net thread finishes on its own (see
// ~HttpClient); no further handler runs.
//
// Redaction: a key that the server reflects is replaced by "[redacted]" in every token, in the
// final error fields and in partialText. Each token is redacted alone, so a key split over two
// tokens passes through the pieces; the consumer that joins the tokens into the displayed text
// must redact the joined text too (the key is a registered secret, redact() finds it).
//
// Cancelling the token ends the request with ChatError::Code::Cancelled and the text received so
// far in partialText; the connection is closed before the error is reported.
class AnthropicProvider final : public providers::Provider {
public:
    AnthropicProvider(net::Endpoint endpoint, SecretString apiKey,
                      AnthropicProviderOptions options = {});
    ~AnthropicProvider() override;

    void streamChat(const providers::ChatRequest& request, providers::StreamHandlers handlers,
                    providers::CancelToken cancel) override;

private:
    net::Endpoint endpoint_;
    std::shared_ptr<const SecretString> key_;
    AnthropicProviderOptions options_;
    int stallSeconds_ = 45;
    // Set first thing in the destructor; the per-request state checks it before every handler call.
    std::shared_ptr<std::atomic<bool>> closing_ = std::make_shared<std::atomic<bool>>(false);
    // Last: destroyed first, which joins the net thread before anything above goes away.
    HttpClient client_;
};

}  // namespace curia::http
