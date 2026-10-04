// SPDX-License-Identifier: MIT
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace curia::platform {

// Returns the value of an environment variable, or an empty string if it is unset.
inline std::string getEnv(const char* name) {
#if defined(_WIN32)
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
        return {};
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

// Like getEnv, but returns a path. On Windows it reads the wide-character variable so profile paths
// with non-ASCII user names survive (the narrow variant is in the ANSI code page).
inline std::filesystem::path getEnvPath(const wchar_t* wideName, const char* narrowName) {
#if defined(_WIN32)
    (void)narrowName;
    wchar_t* buffer = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&buffer, &length, wideName) != 0 || buffer == nullptr) {
        return {};
    }
    std::filesystem::path value(buffer);
    std::free(buffer);
    return value;
#else
    (void)wideName;
    return std::filesystem::path(getEnv(narrowName));
#endif
}

}  // namespace curia::platform
