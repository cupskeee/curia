// SPDX-License-Identifier: MIT
#include <curl/curl.h>
#include <doctest/doctest.h>

#include <optional>
#include <string>

#include "http/tls_policy.h"

using curia::http::TlsPolicy;
using Mode = TlsPolicy::Mode;
using Attempt = TlsPolicy::Attempt;

namespace {

const std::string kBundle = "/test/ca-bundle.pem";

TlsPolicy policy(Mode mode, bool withBundle = true) {
    return TlsPolicy(mode, withBundle ? std::optional<std::string>(kBundle) : std::nullopt);
}

int code(CURLcode value) {
    return static_cast<int>(value);
}

// What an attempt would use: "none" (no attempt), "default" (libcurl's store) or the bundle path.
// Written without dereferencing an optional, so clang-tidy's optional-access check stays quiet.
std::string shape(const std::optional<Attempt>& attempt) {
    if (!attempt.has_value()) {
        return "none";
    }
    const std::optional<std::string>& bundle = attempt.value().caBundle;
    return bundle.has_value() ? bundle.value() : std::string("default");
}

}  // namespace

TEST_CASE("tls-policy: Auto starts with libcurl's default store") {
    CHECK(shape(policy(Mode::Auto).firstAttempt()) == "default");
}

TEST_CASE("tls-policy: Auto retries once with the bundle after a trust failure") {
    const TlsPolicy auto_ = policy(Mode::Auto);
    const Attempt defaultStore{};
    for (const CURLcode failure :
         {CURLE_PEER_FAILED_VERIFICATION, CURLE_SSL_CACERT_BADFILE, CURLE_SSL_CERTPROBLEM}) {
        CAPTURE(code(failure));
        CHECK(shape(auto_.retryAfter(code(failure), defaultStore)) == kBundle);
    }
}

TEST_CASE("tls-policy: nothing but a trust failure is retried") {
    const TlsPolicy auto_ = policy(Mode::Auto);
    const Attempt defaultStore{};
    for (const CURLcode other :
         {CURLE_OK, CURLE_COULDNT_CONNECT, CURLE_COULDNT_RESOLVE_HOST, CURLE_OPERATION_TIMEDOUT,
          CURLE_SSL_CONNECT_ERROR, CURLE_RECV_ERROR, CURLE_GOT_NOTHING, CURLE_WRITE_ERROR}) {
        CAPTURE(code(other));
        CHECK(shape(auto_.retryAfter(code(other), defaultStore)) == "none");
    }
}

TEST_CASE("tls-policy: there is never a third attempt") {
    const TlsPolicy auto_ = policy(Mode::Auto);
    const Attempt withBundle{kBundle};
    CHECK(shape(auto_.retryAfter(code(CURLE_PEER_FAILED_VERIFICATION), withBundle)) == "none");
}

TEST_CASE("tls-policy: no bundle found means no retry") {
    const TlsPolicy none = policy(Mode::Auto, false);
    CHECK(shape(none.retryAfter(code(CURLE_PEER_FAILED_VERIFICATION), Attempt{})) == "none");
    // Nothing to prefer: the first attempt stays on the default store.
    CHECK(shape(none.firstAttempt(true)) == "default");
}

TEST_CASE("tls-policy: DefaultOnly and ProbeOnly") {
    const TlsPolicy defaultOnly = policy(Mode::DefaultOnly);
    CHECK(shape(defaultOnly.firstAttempt()) == "default");
    CHECK(shape(defaultOnly.retryAfter(code(CURLE_PEER_FAILED_VERIFICATION), Attempt{})) == "none");
    CHECK(shape(defaultOnly.firstAttempt(true)) == "default");

    const TlsPolicy probeOnly = policy(Mode::ProbeOnly);
    CHECK(shape(probeOnly.firstAttempt()) == kBundle);
    CHECK(shape(probeOnly.retryAfter(code(CURLE_PEER_FAILED_VERIFICATION), Attempt{kBundle})) ==
          "none");

    // Without a bundle the mode cannot run at all.
    CHECK(shape(policy(Mode::ProbeOnly, false).firstAttempt()) == "none");
}

TEST_CASE("tls-policy: a client that needed the bundle once can start with it") {
    CHECK(shape(policy(Mode::Auto).firstAttempt(true)) == kBundle);
}

TEST_CASE("tls-policy: isCaProblem") {
    CHECK(TlsPolicy::isCaProblem(60));
    CHECK(TlsPolicy::isCaProblem(77));
    CHECK(TlsPolicy::isCaProblem(58));
    CHECK_FALSE(TlsPolicy::isCaProblem(0));
    CHECK_FALSE(TlsPolicy::isCaProblem(7));
    CHECK_FALSE(TlsPolicy::isCaProblem(28));
    CHECK_FALSE(TlsPolicy::isCaProblem(35));
}

TEST_CASE("tls-policy: describe") {
    CHECK(TlsPolicy::describe(Attempt{}) == "libcurl-default");
    CHECK(TlsPolicy::describe(Attempt{kBundle}) == "probed:/test/ca-bundle.pem");
}

TEST_CASE("tls-policy: apply sets the bundle on a handle without error") {
    CURL* easy = curl_easy_init();
    REQUIRE(easy != nullptr);
    const TlsPolicy auto_ = policy(Mode::Auto);
    CHECK(auto_.apply(easy, Attempt{}) == CURLE_OK);
    CHECK(auto_.apply(easy, Attempt{kBundle}) == CURLE_OK);
    curl_easy_cleanup(easy);
}

TEST_CASE("tls-policy: the default constructor is Auto") {
    const TlsPolicy defaulted;
    CHECK((defaulted.mode() == Mode::Auto));
    CHECK((TlsPolicy::system(Mode::DefaultOnly).mode() == Mode::DefaultOnly));
}
