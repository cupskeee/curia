// SPDX-License-Identifier: MIT
#include "net/endpoint_policy.h"

#include <cstddef>

namespace curia::net {

namespace {

#if defined(CURIA_ENABLE_DEV_ENV)

constexpr std::string_view kScheme = "http://";
constexpr std::string_view kLoopbackV6 = "[::1]";
constexpr std::string_view kPathSuffix = "/v1/messages";
constexpr std::string_view kFakeMarker = "FAKE";

bool isDigit(char ch) {
    return ch >= '0' && ch <= '9';
}

// Digits only, no leading zero (a lone "0" is fine), at most `maxDigits` long.
bool parseDecimal(std::string_view text, std::size_t maxDigits, unsigned& value) {
    if (text.empty() || text.size() > maxDigits || (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    unsigned result = 0;
    for (const char ch : text) {
        if (!isDigit(ch)) {
            return false;
        }
        result = result * 10 + static_cast<unsigned>(ch - '0');
    }
    value = result;
    return true;
}

// "127.a.b.c" with a, b, c in 0..255.
bool isLoopbackQuad(std::string_view host) {
    unsigned first = 0;
    std::size_t octets = 0;
    while (octets < 4) {
        const std::size_t dot = host.find('.');
        const bool last = octets == 3;
        if (last != (dot == std::string_view::npos)) {
            return false;  // too many or too few dots
        }
        unsigned octet = 0;
        if (!parseDecimal(host.substr(0, dot), 3, octet) || octet > 255) {
            return false;
        }
        if (octets == 0) {
            first = octet;
        }
        ++octets;
        if (!last) {
            host.remove_prefix(dot + 1);
        }
    }
    return first == 127;
}

// Checks the flag and splits it. Messages never repeat the flag.
bool parseTestEndpoint(std::string_view flag, std::string& host, std::string& port,
                       std::string& error) {
    for (const char ch : flag) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte >= 0x7f || ch == '\\') {
            error = "the test endpoint contains a blank, control, backslash or non-ASCII character";
            return false;
        }
    }
    if (flag.substr(0, kScheme.size()) != kScheme) {
        error = "the test endpoint must start with http:// in lower case";
        return false;
    }
    std::string_view rest = flag.substr(kScheme.size());
    if (!rest.empty() && rest.back() == '/') {
        rest.remove_suffix(1);  // the one allowed path: "/"
    }
    // What is left is the authority and must be made of nothing but a loopback literal and a port:
    // no name, userinfo ('@'), query ('?'), fragment ('#'), path ('/'), escape ('%') or letter.
    for (const char ch : rest) {
        if (!isDigit(ch) && ch != '.' && ch != ':' && ch != '[' && ch != ']') {
            error = "the test endpoint must be http://127.x.y.z:PORT or http://[::1]:PORT";
            return false;
        }
    }
    std::string_view hostPart;
    std::string_view portPart;
    if (!rest.empty() && rest.front() == '[') {
        if (rest.substr(0, kLoopbackV6.size() + 1) != "[::1]:") {
            error = "the only IPv6 host allowed in a test endpoint is [::1], followed by a port";
            return false;
        }
        hostPart = kLoopbackV6;
        portPart = rest.substr(kLoopbackV6.size() + 1);
    } else {
        const std::size_t colon = rest.find(':');
        if (colon == std::string_view::npos) {
            error = "the test endpoint needs an explicit port";
            return false;
        }
        hostPart = rest.substr(0, colon);
        portPart = rest.substr(colon + 1);
        if (!isLoopbackQuad(hostPart)) {
            error = "the test endpoint host must be a loopback address written as 127.x.y.z";
            return false;
        }
    }
    unsigned portNumber = 0;
    if (!parseDecimal(portPart, 5, portNumber) || portNumber < 1 || portNumber > 65535) {
        error = "the test endpoint port must be a number from 1 to 65535 without leading zeros";
        return false;
    }
    host = std::string(hostPart);
    port = std::string(portPart);
    return true;
}

#endif  // CURIA_ENABLE_DEV_ENV

}  // namespace

bool makeEndpoint(std::optional<std::string_view> testEndpointFlag, const SecretString* key,
                  Endpoint& out, std::string& error) {
    if (!testEndpointFlag.has_value()) {
        out = Endpoint{std::string(kAnthropicMessagesUrl), false, false, "api.anthropic.com"};
        return true;
    }
#if defined(CURIA_ENABLE_DEV_ENV)
    std::string host;
    std::string port;
    if (!parseTestEndpoint(*testEndpointFlag, host, port, error)) {
        return false;
    }
    if (key == nullptr || !key->contains(kFakeMarker)) {
        error = "a test endpoint needs a test key: the key must contain FAKE";
        return false;
    }
    out = Endpoint{std::string(kScheme) + host + ":" + port + std::string(kPathSuffix), true, true,
                   host};
    return true;
#else
    (void)key;
    (void)out;
    error = "test endpoints are disabled in this build";
    return false;
#endif
}

bool authMayAttach(std::string_view requestUrl, const Endpoint& endpoint) {
    return !endpoint.url.empty() && requestUrl == endpoint.url;
}

}  // namespace curia::net
