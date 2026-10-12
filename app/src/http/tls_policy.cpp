// SPDX-License-Identifier: MIT
#include "http/tls_policy.h"

#include <utility>

#include "net/ca_bundle.h"

namespace curia::http {

namespace {

std::optional<std::string> locateBundle() {
    if (const auto bundle = net::findSystemCaBundle(); bundle.has_value()) {
        return bundle->string();
    }
    return std::nullopt;
}

}  // namespace

TlsPolicy::TlsPolicy() : TlsPolicy(Mode::Auto, locateBundle()) {}

TlsPolicy::TlsPolicy(Mode mode, std::optional<std::string> probedBundle)
    : mode_(mode), probed_(std::move(probedBundle)) {}

TlsPolicy TlsPolicy::system(Mode mode) {
    return TlsPolicy(mode, locateBundle());
}

std::optional<TlsPolicy::Attempt> TlsPolicy::firstAttempt(bool preferProbed) const {
    switch (mode_) {
    case Mode::DefaultOnly:
        return Attempt{};
    case Mode::ProbeOnly:
        if (!probed_.has_value()) {
            return std::nullopt;
        }
        return Attempt{probed_};
    case Mode::Auto:
        if (preferProbed && probed_.has_value()) {
            return Attempt{probed_};
        }
        return Attempt{};
    }
    return std::nullopt;
}

std::optional<TlsPolicy::Attempt> TlsPolicy::retryAfter(int curlCode,
                                                        const Attempt& previous) const {
    if (mode_ != Mode::Auto || previous.caBundle.has_value() || !probed_.has_value()) {
        return std::nullopt;
    }
    if (curlCode == CURLE_OK || !isCaProblem(curlCode)) {
        return std::nullopt;
    }
    return Attempt{probed_};
}

bool TlsPolicy::isCaProblem(int curlCode) noexcept {
    return curlCode == static_cast<int>(CURLE_PEER_FAILED_VERIFICATION) ||
           curlCode == static_cast<int>(CURLE_SSL_CACERT_BADFILE) ||
           curlCode == static_cast<int>(CURLE_SSL_CERTPROBLEM);
}

CURLcode TlsPolicy::apply(CURL* easy, const Attempt& attempt) const {
    CURLcode result = CURLE_OK;
    if (attempt.caBundle.has_value()) {
        result = curl_easy_setopt(easy, CURLOPT_CAINFO, attempt.caBundle->c_str());
    }
#if defined(_WIN32) && defined(CURLSSLOPT_REVOKE_BEST_EFFORT)
    const CURLcode options = curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS,
                                              static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    if (result == CURLE_OK) {
        result = options;
    }
#endif
    return result;
}

std::string TlsPolicy::describe(const Attempt& attempt) {
    if (attempt.caBundle.has_value()) {
        return "probed:" + *attempt.caBundle;
    }
    return "libcurl-default";
}

}  // namespace curia::http
