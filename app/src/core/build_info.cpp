// SPDX-License-Identifier: MIT
#include "core/build_info.h"

#include <nlohmann/json.hpp>

#include "curia/version.h"

namespace curia {

namespace {

constexpr std::string_view compilerName() {
#if defined(__clang__)
    return "clang";
#elif defined(__GNUC__)
    return "gcc";
#elif defined(_MSC_VER)
    return "msvc";
#else
    return "unknown";
#endif
}

constexpr std::string_view platformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

}  // namespace

std::string_view versionString() {
    return CURIA_VERSION_STRING;
}

std::string buildInfoJson() {
    nlohmann::json info;
    info["name"] = "curia";
    info["version"] = std::string(versionString());
    info["compiler"] = std::string(compilerName());
    info["platform"] = std::string(platformName());
    return info.dump();
}

}  // namespace curia
