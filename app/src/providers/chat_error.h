// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string_view>

#include "providers/chat_types.h"

// Turning what the Anthropic API says about a failure into a ChatError. Pure functions; the
// transport failures that exist only with libcurl (DNS, TLS, timeouts) are mapped in curia_http.
//
// Messages carry the server's own text, cut to a bounded length at a character boundary, with
// control characters made spaces. They are not redacted here: the caller passes them through
// redact() before they reach a log or the screen.
namespace curia::providers {

// The longest server message kept, in bytes.
inline constexpr std::size_t kMaxServerMessageBytes = 300;
// The longest excerpt of a body that is not the expected JSON error, in bytes.
inline constexpr std::size_t kMaxBodyExcerptBytes = 200;
// The longest request id kept, in bytes (from a header or from a body; cleaned like a message).
inline constexpr std::size_t kMaxRequestIdBytes = 128;

// An HTTP response with a non-2xx status.
//   400, 413, other 4xx  BadRequest, not retryable    401, 403  Auth, not retryable
//   404                  BadRequest, not retryable, message says the model may not exist
//   408                  Network, retryable            429       RateLimit, retryable
//   500                  Other, retryable              502, 503, 504  Network, retryable
//   529                  Overloaded, retryable         other 5xx Other, retryable
//   3xx                  Network, not retryable ("unexpected redirect": a redirect is never
//                        followed, so that the key cannot be sent anywhere else)
//   anything else        Other, not retryable
// If the body is the API's JSON error ({"type":"error","error":{"type":T,"message":M}}) and T is
// one of the types mapStreamError() knows, T decides code and retryable (the status can be
// generic, 500 for an overloaded server, say) and M is the message; otherwise the message is the
// body's excerpt, or a plain description of the status. `retryAfterHeader` is the value of the
// Retry-After header, or empty: only digits count (seconds, 0 to 86400); the HTTP-date form and
// anything else is ignored. httpStatus, retryAfterSeconds and requestId are filled in.
ChatError mapHttpError(int status, std::string_view body, std::string_view retryAfterHeader,
                       std::string_view requestId);

// An `event: error` of the stream (sent after a 200), by the error type word:
//   overloaded_error  Overloaded, retryable    rate_limit_error  RateLimit, retryable
//   authentication_error, permission_error  Auth    invalid_request_error, not_found_error,
//   request_too_large  BadRequest    api_error  Other, retryable    timeout_error  Network,
//   retryable    anything else  Other, not retryable
// httpStatus is 0. An empty message becomes a plain description.
ChatError mapStreamError(std::string_view type, std::string_view message);

}  // namespace curia::providers
