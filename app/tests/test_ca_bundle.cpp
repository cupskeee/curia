// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <filesystem>

#include "net/ca_bundle.h"

TEST_CASE("CA bundle candidates are consistent with the platform") {
    const auto candidates = curia::net::candidateCaBundlePaths();
#if defined(_WIN32)
    CHECK(candidates.empty());  // Schannel uses the Windows certificate store
#else
    CHECK_FALSE(candidates.empty());
#endif
    const auto found = curia::net::findSystemCaBundle();
    if (found.has_value()) {
        CHECK(std::filesystem::is_regular_file(*found));
    }
}
