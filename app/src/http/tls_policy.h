// SPDX-License-Identifier: MIT
#pragma once

#include <curl/curl.h>

#include <optional>
#include <string>

namespace curia::http {

// How the TLS trust store is chosen for a request (milestone M1, A16). vcpkg's libcurl uses OpenSSL
// on macOS and Linux, which may have no default CA store on a given machine; on Windows libcurl
// uses Schannel and the Windows certificate store. The policy is: try libcurl's own default first;
// if that attempt fails with a certificate-trust error and a system CA bundle was found
// (net/ca_bundle.h), retry once with CURLOPT_CAINFO pointing at it. Verification is never turned
// off and no other failure is retried.
//
// The decisions are plain functions of the policy's state, so they are tested without a network;
// only apply() touches a libcurl handle.
class TlsPolicy {
public:
    enum class Mode {
        // Default first, a located bundle only after a trust failure (what the app does).
        Auto,
        // libcurl's default only (records what the platform does on its own).
        DefaultOnly,
        // The located bundle only.
        ProbeOnly,
    };

    // One try at a request: libcurl's default store (no bundle) or an explicit bundle file.
    struct Attempt {
        std::optional<std::string> caBundle;
        bool operator==(const Attempt&) const = default;
    };

    // Auto, with a bundle found by findSystemCaBundle().
    TlsPolicy();
    // Explicit state, for tests and for tools that choose the mode.
    TlsPolicy(Mode mode, std::optional<std::string> probedBundle);
    // The mode with a bundle found by findSystemCaBundle().
    static TlsPolicy system(Mode mode = Mode::Auto);

    Mode mode() const noexcept { return mode_; }
    const std::optional<std::string>& probedBundle() const noexcept { return probed_; }

    // The first attempt, or nullopt when the mode cannot run (ProbeOnly with no bundle found).
    // `preferProbed` says an earlier request in this process already needed the bundle, so a
    // failing default attempt is skipped (only in Auto mode, and only if a bundle exists).
    std::optional<Attempt> firstAttempt(bool preferProbed = false) const;

    // What to try after `previous` ended with `curlCode` (a CURLcode as int): the bundle attempt if
    // this is Auto mode, `previous` used the default store, the failure is a certificate-trust
    // problem and a bundle exists; nullopt otherwise (so there is never a third attempt).
    std::optional<Attempt> retryAfter(int curlCode, const Attempt& previous) const;

    // True for the errors that mean "the certificate chain could not be trusted with this store".
    static bool isCaProblem(int curlCode) noexcept;

    // Sets the CA bundle (if the attempt has one) and, on Windows, best-effort revocation checking
    // (Schannel fails hard when a revocation server is unreachable; the chain is still verified).
    // Returns the first curl error, CURLE_OK otherwise.
    CURLcode apply(CURL* easy, const Attempt& attempt) const;

    // "libcurl-default" or "probed:<path>", for tool output.
    static std::string describe(const Attempt& attempt);

private:
    Mode mode_ = Mode::Auto;
    std::optional<std::string> probed_;
};

}  // namespace curia::http
