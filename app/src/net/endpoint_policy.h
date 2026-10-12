// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/secret.h"

namespace curia::net {

// Where the API key may be sent. Decision D19: the only endpoint is Anthropic's, except that a
// build with CURIA_ENABLE_DEV_ENV accepts one command-line flag, --test-endpoint, that points the
// app at a mock server on this machine, and then only with a key that says it is fake. No
// environment variable, setting or file can redirect the key.
struct Endpoint {
    // The full request URL (scheme, host, port, path "/v1/messages").
    std::string url;
    // True for a --test-endpoint.
    bool isTest = false;
    // True if the transport must not use any proxy for this endpoint (test endpoints: a proxy
    // configured in the environment must never see a request meant for loopback).
    bool disableProxy = false;
    // The host as written in the URL: "api.anthropic.com", "127.0.0.1" or "[::1]" (no port).
    std::string host;
};

inline constexpr std::string_view kAnthropicMessagesUrl = "https://api.anthropic.com/v1/messages";

// Builds the endpoint. Without a flag: Anthropic's, https, proxies honoured. With one:
//   * the build must have CURIA_ENABLE_DEV_ENV, else the error is "test endpoints are disabled in
//     this build";
//   * the flag must be exactly "http://" + a loopback literal + ":" + a port, with at most one
//     trailing "/": the host is a canonical dotted quad whose first octet is 127 (four decimal
//     octets, no leading zeros) or the literal [::1]; the port is 1..65535 without leading zeros.
//     No userinfo, query, fragment, path, name, percent escape, blank, backslash, control or
//     non-ASCII byte is accepted (a parser that is tolerant here is the usual way a key leaks to
//     the wrong host);
//   * the key must be present and contain "FAKE" (case-sensitive).
// The URL becomes the flag without the trailing "/" plus "/v1/messages".
//
// On failure returns false with a message in `error` (it names the rule, never repeats the flag or
// the key) and leaves `out` untouched.
bool makeEndpoint(std::optional<std::string_view> testEndpointFlag, const SecretString* key,
                  Endpoint& out, std::string& error);

// True only if `requestUrl` is exactly the endpoint's URL. The transport asserts this before it
// attaches the key to a request, so a redirect target, a typo or a future code path that builds
// another URL can never carry the key.
bool authMayAttach(std::string_view requestUrl, const Endpoint& endpoint);

}  // namespace curia::net
