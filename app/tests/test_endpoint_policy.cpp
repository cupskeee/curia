// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "net/endpoint_policy.h"

using curia::SecretString;
using curia::net::Endpoint;
using curia::net::isLoopbackTestUrl;
using curia::net::makeEndpoint;

namespace {

SecretString fakeKey() {
    return SecretString(std::string("sk-ant-") + "FAKE" + "0123456789abcdef");
}

SecretString plainKey() {
    return SecretString(std::string("sk-ant-") + "plain" + "0123456789abcdef");
}

struct Result {
    bool ok = false;
    Endpoint endpoint;
    std::string error;
};

Result tryEndpoint(const std::string& flag, const SecretString* key) {
    Result result;
    result.ok = makeEndpoint(std::string_view(flag), key, result.endpoint, result.error);
    return result;
}

}  // namespace

TEST_CASE("endpoint: without a flag it is Anthropic over https with proxies allowed") {
    Endpoint endpoint;
    std::string error;
    const SecretString key = plainKey();
    REQUIRE(makeEndpoint(std::nullopt, &key, endpoint, error));
    CHECK(endpoint.url == "https://api.anthropic.com/v1/messages");
    CHECK(endpoint.host == "api.anthropic.com");
    CHECK_FALSE(endpoint.isTest);
    CHECK_FALSE(endpoint.disableProxy);
    CHECK(error.empty());

    // The key does not matter (it may not exist yet).
    Endpoint again;
    REQUIRE(makeEndpoint(std::nullopt, nullptr, again, error));
    CHECK(again.url == endpoint.url);
}

TEST_CASE("endpoint: authMayAttach is an exact comparison") {
    Endpoint endpoint;
    std::string error;
    REQUIRE(makeEndpoint(std::nullopt, nullptr, endpoint, error));
    CHECK(curia::net::authMayAttach("https://api.anthropic.com/v1/messages", endpoint));
    CHECK_FALSE(curia::net::authMayAttach("https://api.anthropic.com/v1/messages/", endpoint));
    CHECK_FALSE(curia::net::authMayAttach("https://api.anthropic.com/v1/messages?x=1", endpoint));
    CHECK_FALSE(curia::net::authMayAttach("https://api.anthropic.com/v1/models", endpoint));
    CHECK_FALSE(curia::net::authMayAttach("http://api.anthropic.com/v1/messages", endpoint));
    CHECK_FALSE(curia::net::authMayAttach("https://API.anthropic.com/v1/messages", endpoint));
    CHECK_FALSE(
        curia::net::authMayAttach("https://api.anthropic.com.evil.test/v1/messages", endpoint));
    CHECK_FALSE(curia::net::authMayAttach("", endpoint));
    CHECK_FALSE(curia::net::authMayAttach("https://api.anthropic.com/v1/messages", Endpoint{}));
    CHECK_FALSE(curia::net::authMayAttach("", Endpoint{}));
}

#if defined(CURIA_ENABLE_DEV_ENV)

TEST_CASE("endpoint: accepted test endpoints") {
    const SecretString key = fakeKey();
    struct Case {
        const char* flag;
        const char* url;
        const char* host;
    };
    const std::vector<Case> accepted = {
        {"http://127.0.0.1:8080", "http://127.0.0.1:8080/v1/messages", "127.0.0.1"},
        {"http://127.0.0.1:8080/", "http://127.0.0.1:8080/v1/messages", "127.0.0.1"},
        {"http://127.1.2.3:1", "http://127.1.2.3:1/v1/messages", "127.1.2.3"},
        {"http://127.255.255.255:65535", "http://127.255.255.255:65535/v1/messages",
         "127.255.255.255"},
        {"http://[::1]:65535", "http://[::1]:65535/v1/messages", "[::1]"},
        {"http://[::1]:8080/", "http://[::1]:8080/v1/messages", "[::1]"},
        {"http://127.0.0.0:9", "http://127.0.0.0:9/v1/messages", "127.0.0.0"},
    };
    for (const Case& c : accepted) {
        INFO(c.flag);
        const Result result = tryEndpoint(c.flag, &key);
        CHECK(result.ok);
        CHECK(result.endpoint.url == c.url);
        CHECK(result.endpoint.host == c.host);
        CHECK(result.endpoint.isTest);
        CHECK(result.endpoint.disableProxy);
        CHECK(result.error.empty());
        CHECK(curia::net::authMayAttach(c.url, result.endpoint));
        CHECK_FALSE(curia::net::authMayAttach(std::string(c.url) + "x", result.endpoint));
    }
}

TEST_CASE("endpoint: rejected test endpoints") {
    const SecretString key = fakeKey();
    const std::vector<std::string> rejected = {
        // userinfo and host confusion
        "http://127.0.0.1@evil.com",
        "http://127.0.0.1@evil.com:80",
        "http://127.0.0.1:80@evil.com",
        "http://127.0.0.1.evil.com",
        "http://127.0.0.1.evil.com:80",
        "http://evil.com#@127.0.0.1",
        "http://evil.com#@127.0.0.1:80",
        "http://evil.com:80#@127.0.0.1:80",
        "http://127.0.0.1:80\\@evil",
        "http://127.0.0.1\\@evil:80",
        // names and alternative spellings of loopback
        "http://localhost:80",
        "http://0x7f.1:80",
        "http://0x7f000001:80",
        "http://2130706433:80",
        "http://127.1:80",
        "http://127.0.1:80",
        "http://017700000001:80",
        "http://0.0.0.0:80",
        "http://127.0.0.1.:80",
        "http://127.0.0.1..:80",
        "http://.127.0.0.1:80",
        "http://127.000.000.001:80",
        "http://127.00.0.1:80",
        "http://127.0.0.01:80",
        "http://127.0.0.256:80",
        "http://127.0.0.1000:80",
        "http://127.-1.0.1:80",
        "http://127.0.0.1.2:80",
        "http://128.0.0.1:80",
        "http://126.255.255.255:80",
        "http://10.0.0.1:80",
        "http://192.168.0.1:80",
        "http://1.1.1.1:80",
        // scheme
        "https://127.0.0.1:80",
        "HTTP://127.0.0.1:80",
        "Http://127.0.0.1:80",
        "ftp://127.0.0.1:80",
        "127.0.0.1:80",
        "//127.0.0.1:80",
        "http:/127.0.0.1:80",
        "http:127.0.0.1:80",
        "http//127.0.0.1:80",
        "",
        "http://",
        // ports
        "http://127.0.0.1",
        "http://127.0.0.1:",
        "http://127.0.0.1:0",
        "http://127.0.0.1:00",
        "http://127.0.0.1:080",
        "http://127.0.0.1:99999",
        "http://127.0.0.1:65536",
        "http://127.0.0.1:123456",
        "http://127.0.0.1:-1",
        "http://127.0.0.1:+80",
        "http://127.0.0.1:8o",
        "http://127.0.0.1:80:90",
        "http://[::1]",
        "http://[::1]:",
        "http://[::1]:0",
        "http://[::1]:65536",
        // IPv6 variants
        "http://[::1]x:80",
        "http://[::1",
        "http://::1:80",
        "http://[::2]:80",
        "http://[0:0:0:0:0:0:0:1]:80",
        "http://[::ffff:127.0.0.1]:80",
        "http://[::ffff:7f00:1]:80",
        "http://[::1]%25eth0:80",
        "http://[[::1]]:80",
        // path, query, fragment, userinfo
        "http://127.0.0.1:80/v1/messages",
        "http://127.0.0.1:80/x",
        "http://127.0.0.1:80//",
        "http://127.0.0.1:80?x=1",
        "http://127.0.0.1:80/?x=1",
        "http://127.0.0.1:80#frag",
        "http://127.0.0.1:80/#frag",
        "http://user@127.0.0.1:80",
        "http://user:pw@127.0.0.1:80",
        "http://@127.0.0.1:80",
        // characters that must never appear
        "http://127.0.0.1:80 ",
        " http://127.0.0.1:80",
        "http://127.0.0.1: 80",
        "http://127.0.0.1:80\t",
        "http://127.0.0.1:80\r",
        "http://127.0.0.1:80\n",
        "http://127.0.0.1:80\r\nHost: evil",
        "http://127.0.0.1:80\n/",
        std::string("http://127.0.0.1:80\0", 20),
        std::string("http://127.0.0.1\0:80", 20),
        std::string("http://127.0.0.1:80\0@evil", 25),
        "http://127.0.0.1:80\\",
        "http:\\\\127.0.0.1:80",
        "http://127.0.0.1%3a80",
        "http://127%2e0.0.1:80",
        "http://%31%32%37.0.0.1:80",
        "http://127.0.0.1:%38%30",
        "http://\xef\xbc\x91\xef\xbc\x92\xef\xbc\x97.0.0.1:80",  // full-width digits
        "http://127.0.0.\xd9\xa1:80",                            // Arabic-Indic digit one
        "http://127.0.0.1:80\xc2\xa0",
        "http://127.0.0.1:\xef\xbc\x98\xef\xbc\x90",
        "http://127.0.0.1:80\x7f",
        "http://127.0.0.1\x01:80",
    };
    for (const std::string& flag : rejected) {
        INFO("flag of " << flag.size() << " bytes");
        Result result;
        result.endpoint.url = "unchanged";
        result.ok = makeEndpoint(std::string_view(flag), &key, result.endpoint, result.error);
        CHECK_FALSE(result.ok);
        CHECK_FALSE(result.error.empty());
        CHECK(result.endpoint.url == "unchanged");  // untouched on failure
        // The messages never repeat the flag or the key.
        CHECK(result.error.find("0123456789abcdef") == std::string::npos);
        CHECK(result.error.find("sk-ant") == std::string::npos);
        CHECK(result.error.find('\n') == std::string::npos);
        CHECK(result.error.find('\r') == std::string::npos);
    }
}

TEST_CASE("endpoint: a test endpoint needs a key that says it is fake") {
    const std::string flag = "http://127.0.0.1:8080";

    const Result none = tryEndpoint(flag, nullptr);
    CHECK_FALSE(none.ok);
    CHECK(none.error.find("FAKE") != std::string::npos);

    const SecretString plain = plainKey();
    const Result notFake = tryEndpoint(flag, &plain);
    CHECK_FALSE(notFake.ok);
    CHECK(notFake.error.find("0123456789abcdef") == std::string::npos);
    CHECK(notFake.error.find("sk-ant") == std::string::npos);

    const SecretString lower(std::string("sk-ant-") + "fake" + "0123456789abcdef");
    CHECK_FALSE(tryEndpoint(flag, &lower).ok);  // case-sensitive

    const SecretString empty;
    CHECK_FALSE(tryEndpoint(flag, &empty).ok);

    const SecretString fake = fakeKey();
    CHECK(tryEndpoint(flag, &fake).ok);

    // "FAKE" anywhere in the key is enough, including as the whole key.
    const SecretString onlyMarker{std::string("FAKE")};
    CHECK(tryEndpoint(flag, &onlyMarker).ok);
}

TEST_CASE("endpoint: a test endpoint is ignored by a plain run") {
    // The default endpoint never depends on the key, so a real key with no flag is Anthropic's.
    Endpoint endpoint;
    std::string error;
    const SecretString key = fakeKey();
    REQUIRE(makeEndpoint(std::nullopt, &key, endpoint, error));
    CHECK_FALSE(endpoint.isTest);
    CHECK(endpoint.url == "https://api.anthropic.com/v1/messages");
}

TEST_CASE("endpoint: isLoopbackTestUrl accepts only what makeEndpoint builds") {
    for (const char* url : {"http://127.0.0.1:8080/v1/messages", "http://127.1.2.3:1/v1/messages",
                            "http://[::1]:65535/v1/messages"}) {
        CAPTURE(url);
        CHECK(isLoopbackTestUrl(url));
    }
    for (const char* url : {"",
                            "http://127.0.0.1:8080",
                            "http://127.0.0.1:8080/",
                            "http://127.0.0.1:8080//v1/messages",
                            "http://127.0.0.1:8080/v1/messages/",
                            "http://127.0.0.1:8080/v1/other",
                            "https://127.0.0.1:8080/v1/messages",
                            "HTTP://127.0.0.1:8080/v1/messages",
                            "http://evil.example.com:80/v1/messages",
                            "http://localhost:80/v1/messages",
                            "http://128.0.0.1:80/v1/messages",
                            "http://127.0.0.1/v1/messages",
                            "http://127.0.0.1:0/v1/messages",
                            "http://127.0.0.1:80@evil.example/v1/messages",
                            "http://user@127.0.0.1:80/v1/messages",
                            "http://127.0.0.1:80/v1/messages?x=1",
                            "http://[::2]:80/v1/messages",
                            "http://0x7f.0.0.1:80/v1/messages",
                            "http://127.0.0.1:80/x/v1/messages",
                            "/v1/messages"}) {
        CAPTURE(url);
        CHECK_FALSE(isLoopbackTestUrl(url));
    }
    // What makeEndpoint builds passes.
    const SecretString key = fakeKey();
    const Result result = tryEndpoint("http://127.0.0.1:4321/", &key);
    REQUIRE(result.ok);
    CHECK(isLoopbackTestUrl(result.endpoint.url));
}

#else  // built without CURIA_ENABLE_DEV_ENV

TEST_CASE("endpoint: test endpoints are disabled in this build, whatever the flag and key") {
    const SecretString key = fakeKey();
    for (const char* flag : {"http://127.0.0.1:8080", "", "https://api.anthropic.com", "x"}) {
        const Result result = tryEndpoint(flag, &key);
        CHECK_FALSE(result.ok);
        CHECK(result.error == "test endpoints are disabled in this build");
        CHECK(result.endpoint.url.empty());
    }
    const Result noKey = tryEndpoint("http://127.0.0.1:8080", nullptr);
    CHECK_FALSE(noKey.ok);
    CHECK(noKey.error == "test endpoints are disabled in this build");
}

TEST_CASE("endpoint: isLoopbackTestUrl is always false in this build") {
    CHECK_FALSE(isLoopbackTestUrl("http://127.0.0.1:8080/v1/messages"));
    CHECK_FALSE(isLoopbackTestUrl("http://[::1]:8080/v1/messages"));
}

#endif
