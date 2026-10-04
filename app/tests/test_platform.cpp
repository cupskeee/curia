// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "platform/platform.h"

TEST_CASE("the platform layer names itself") {
    const auto platform = curia::platform::create();
    REQUIRE(platform != nullptr);
    const auto name = platform->name();
    CHECK((name == "windows" || name == "macos" || name == "linux"));
}

TEST_CASE("candidate CK3 user folders end with Crusader Kings III") {
    const auto platform = curia::platform::create();
    // HOME / USERPROFILE exist on every machine that runs the tests, so there is at least one.
    const auto dirs = platform->candidateCk3UserDirs();
    CHECK_FALSE(dirs.empty());
    for (const auto& dir : dirs) {
        CHECK(dir.filename() == "Crusader Kings III");
    }
}
