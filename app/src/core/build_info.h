// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace curia {

// The app version (SemVer), from CMake project(VERSION).
std::string_view versionString();

// A compact JSON description of this build (name, version, compiler, platform), used by --version.
std::string buildInfoJson();

}  // namespace curia
