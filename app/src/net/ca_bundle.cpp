// SPDX-License-Identifier: MIT
#include "net/ca_bundle.h"

#include <system_error>

namespace curia::net {

std::vector<std::filesystem::path> candidateCaBundlePaths() {
#if defined(_WIN32)
    return {};
#else
    return {
        "/etc/ssl/cert.pem",                           // macOS, Alpine, some BSDs
        "/etc/ssl/certs/ca-certificates.crt",          // Debian, Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",            // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                      // openSUSE
        "/etc/pki/tls/cacert.pem",                     // older RHEL
        "/opt/homebrew/etc/ca-certificates/cert.pem",  // Homebrew (Apple silicon)
        "/usr/local/etc/ca-certificates/cert.pem",     // Homebrew (Intel)
    };
#endif
}

std::optional<std::filesystem::path> findSystemCaBundle() {
    for (const auto& path : candidateCaBundlePaths()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec) && !ec) {
            return path;
        }
    }
    return std::nullopt;
}

}  // namespace curia::net
