# SPDX-License-Identifier: MIT
# Release-only triplet for Linux x86_64: static libraries, dynamic glibc (floor: Ubuntu 24.04).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
