// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>

#include <nlohmann/json.hpp>

#include "core/build_info.h"

TEST_CASE("version string looks like SemVer") {
    const std::string_view version = curia::versionString();
    CHECK_FALSE(version.empty());
    CHECK(std::count(version.begin(), version.end(), '.') == 2);
}

TEST_CASE("build info is valid JSON with the expected fields") {
    const auto info = nlohmann::json::parse(curia::buildInfoJson());
    CHECK(info.at("name") == "curia");
    CHECK(info.at("version") == std::string(curia::versionString()));
    CHECK(info.contains("compiler"));
    CHECK(info.contains("platform"));
}
