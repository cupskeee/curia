// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace curia::net {

// Well-known locations of the system CA bundle. vcpkg's libcurl uses OpenSSL on macOS and Linux,
// which has no system trust store of its own, so we point CURLOPT_CAINFO at one of these. Empty on
// Windows, where libcurl uses Schannel and the Windows certificate store. Verification is never
// disabled.
std::vector<std::filesystem::path> candidateCaBundlePaths();

// The first candidate that exists as a regular file, if any.
std::optional<std::filesystem::path> findSystemCaBundle();

}  // namespace curia::net
