// SPDX-License-Identifier: MIT
// Proves that TLS verification works with the libcurl configuration we ship (milestone M1, A16):
// makes one HTTPS HEAD request and succeeds if the TLS handshake and certificate verification pass
// (any HTTP status counts). No credentials are sent. Verification is never disabled.
//
// Usage: curia_https_smoke [--default-ca | --probe-ca] [url]
//   (no flag)    libcurl's default CA handling first; if that fails with a CA error, retry once
//   with a
//                probed system CA bundle (http/tls_policy.h, the policy the app's HTTP client uses)
//   --default-ca only libcurl's default (records what the platform does on its own)
//   --probe-ca   only the probed CA bundle
#include <curl/curl.h>

#include <array>
#include <cstdio>
#include <string>

#include "http/tls_policy.h"

namespace {

using curia::http::TlsPolicy;

struct Result {
    CURLcode result = CURLE_OK;
    long httpStatus = 0;
    std::string error;
};

size_t discardBody(char* /*data*/, size_t size, size_t count, void* /*user*/) {
    return size * count;
}

Result perform(const std::string& url, const TlsPolicy& policy, const TlsPolicy::Attempt& attempt) {
    Result outcome;
    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        outcome.result = CURLE_FAILED_INIT;
        outcome.error = "curl_easy_init failed";
        return outcome;
    }
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer.data());
    policy.apply(handle, attempt);
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discardBody);
    outcome.result = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &outcome.httpStatus);
    outcome.error =
        errorBuffer[0] != '\0' ? errorBuffer.data() : curl_easy_strerror(outcome.result);
    curl_easy_cleanup(handle);
    return outcome;
}

}  // namespace

int main(int argc, char** argv) {
    TlsPolicy::Mode mode = TlsPolicy::Mode::Auto;
    std::string url = "https://github.com/";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--default-ca") {
            mode = TlsPolicy::Mode::DefaultOnly;
        } else if (arg == "--probe-ca") {
            mode = TlsPolicy::Mode::ProbeOnly;
        } else {
            url = arg;
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    const TlsPolicy policy = TlsPolicy::system(mode);

    const auto first = policy.firstAttempt();
    if (!first.has_value()) {
        std::fprintf(stderr, "HTTPS_SMOKE_FAIL no system CA bundle found to probe\n");
        curl_global_cleanup();
        return 2;
    }
    Result outcome = perform(url, policy, *first);
    std::string strategy = TlsPolicy::describe(*first);
    if (const auto retry = policy.retryAfter(static_cast<int>(outcome.result), *first)) {
        const std::string bundle = retry->caBundle.value_or(std::string("the default store"));
        std::fprintf(stderr, "default CA handling failed (%s); retrying with %s\n",
                     outcome.error.c_str(), bundle.c_str());
        outcome = perform(url, policy, *retry);
        strategy = TlsPolicy::describe(*retry);
    }
    curl_global_cleanup();

    if (outcome.result != CURLE_OK) {
        std::fprintf(stderr, "HTTPS_SMOKE_FAIL curl error %d: %s (strategy: %s)\n",
                     static_cast<int>(outcome.result), outcome.error.c_str(), strategy.c_str());
        return 2;
    }
    std::printf("HTTPS_SMOKE_OK http=%ld strategy=%s\n", outcome.httpStatus, strategy.c_str());
    return 0;
}
