# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)

# Keep the version in sync with vcpkg.json ("version-semver"); the release workflow checks the tag against it.
project(curia VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT on Windows, matching the curia-x64-windows vcpkg triplet.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(CURIA_BUILD_APP "Build the desktop app (needs SDL3, Dear ImGui, libcurl)" ON)
option(CURIA_BUILD_TESTS "Build the unit tests" ON)
option(CURIA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CURIA_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (Clang/GCC)" OFF)

include(CTest)

add_subdirectory(app)
