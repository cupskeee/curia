# SPDX-License-Identifier: MIT
# Release-only static triplet for macOS x86_64 (cross-built on arm64 runners, merged with lipo).
# vcpkg's own x64-osx triplet is a community triplet, so we keep a maintained overlay.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 15.0)
set(VCPKG_BUILD_TYPE release)
