// SPDX-License-Identifier: MIT
// Proves that TLS verification works with the libcurl configuration we ship (milestone M1, A16):
// makes one HTTPS HEAD request and succeeds if the TLS handshake and certificate verification pass
// (any HTTP status counts). No credentials are sent. Verification is never disabled.
//
// Usage: curia_https_smoke [--default-ca | --probe-ca] [url]
//   (no flag)    libcurl's default CA handling first; if that fails with a CA error, retry once
//   with a
//                probed system CA bundle (net/ca_bundle.h)
//   --default-ca only libcurl's default (records what the platform does on its own)
//   --probe-ca   only the probed CA bundle
#include <curl/curl.h>

#include <array>
#include <cstdio>
#include <optional>
#include <string>

#include "net/ca_bundle.h"

namespace {

enum class CaMode { Auto, DefaultOnly, ProbeOnly };

struct Attempt {
    CURLcode result = CURLE_OK;
    long httpStatus = 0;
    std::string error;
};

size_t discardBody(char* /*data*/, size_t size, size_t count, void* /*user*/) {
    return size * count;
}

Attempt perform(const std::string& url, const std::optional<std::string>& caBundle) {
    Attempt attempt;
    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        attempt.result = CURLE_FAILED_INIT;
        attempt.error = "curl_easy_init failed";
        return attempt;
    }
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer.data());
    if (caBundle.has_value()) {
        curl_easy_setopt(handle, CURLOPT_CAINFO, caBundle->c_str());
    }
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discardBody);
#if defined(_WIN32) && defined(CURLSSLOPT_REVOKE_BEST_EFFORT)
    // Schannel fails hard if a certificate revocation server is unreachable; verification of the
    // certificate chain itself stays fully on.
    curl_easy_setopt(handle, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
#endif
    attempt.result = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &attempt.httpStatus);
    attempt.error =
        errorBuffer[0] != '\0' ? errorBuffer.data() : curl_easy_strerror(attempt.result);
    curl_easy_cleanup(handle);
    return attempt;
}

bool isCaProblem(CURLcode code) {
    return code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE ||
           code == CURLE_SSL_CERTPROBLEM;
}

}  // namespace

int main(int argc, char** argv) {
    CaMode mode = CaMode::Auto;
    std::string url = "https://github.com/";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--default-ca") {
            mode = CaMode::DefaultOnly;
        } else if (arg == "--probe-ca") {
            mode = CaMode::ProbeOnly;
        } else {
            url = arg;
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::optional<std::string> probed;
    if (const auto bundle = curia::net::findSystemCaBundle(); bundle.has_value()) {
        probed = bundle->string();
    }

    Attempt attempt;
    std::string strategy;
    if (mode == CaMode::ProbeOnly) {
        if (!probed.has_value()) {
            std::fprintf(stderr, "HTTPS_SMOKE_FAIL no system CA bundle found to probe\n");
            curl_global_cleanup();
            return 2;
        }
        attempt = perform(url, probed);
        strategy = "probed:" + *probed;
    } else {
        attempt = perform(url, std::nullopt);
        strategy = "libcurl-default";
        if (mode == CaMode::Auto && attempt.result != CURLE_OK && isCaProblem(attempt.result) &&
            probed.has_value()) {
            std::fprintf(stderr, "default CA handling failed (%s); retrying with %s\n",
                         attempt.error.c_str(), probed->c_str());
            attempt = perform(url, probed);
            strategy = "probed:" + *probed;
        }
    }
    curl_global_cleanup();

    if (attempt.result != CURLE_OK) {
        std::fprintf(stderr, "HTTPS_SMOKE_FAIL curl error %d: %s (strategy: %s)\n",
                     static_cast<int>(attempt.result), attempt.error.c_str(), strategy.c_str());
        return 2;
    }
    std::printf("HTTPS_SMOKE_OK http=%ld strategy=%s\n", attempt.httpStatus, strategy.c_str());
    return 0;
}
