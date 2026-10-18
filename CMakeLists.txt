# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)

# Keep the version in sync with vcpkg.json ("version-semver"); the release workflow checks the tag against it.
project(curia VERSION 0.1.0 LANGUAGES CXX)

# Objective-C++ for the macOS code: platform/mac, the overlay panel and the hot key (ui/*_mac.mm),
# and the M2 overlay probe.
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

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
option(CURIA_BUILD_FUZZ "Build the libFuzzer targets curia_fuzz_bridge and curia_fuzz_sse (Clang with libFuzzer; use with CURIA_SANITIZE)" OFF)
# Development configuration (decision D19): read CURIA_ANTHROPIC_API_KEY / CURIA_MODEL from the
# environment and accept the --test-endpoint flag. ON in development and CI; turn it OFF for release builds.
option(CURIA_ENABLE_DEV_ENV "Development configuration: env-var API key and --test-endpoint (OFF for release builds, D19)" ON)

# CI turns this ON so that a missing Python 3 fails the configuration instead of silently dropping the
# end-to-end tests that need it.
option(CURIA_REQUIRE_PYTHON "Fail the configuration if Python 3 is missing (otherwise the end-to-end tests are skipped)" OFF)
# The tests that open a window (smoke and idle) can take the keyboard while they run, so they are
# registered only on request (owner decision 2026-10-18); CI turns them on.
option(CURIA_GUI_TESTS "Register the tests that open a window (app_smoke*, app_idle*)" OFF)

include(CTest)

add_subdirectory(app)
