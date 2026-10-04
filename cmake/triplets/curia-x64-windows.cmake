# SPDX-License-Identifier: MIT
# Release-only triplet for Windows x64: static libraries and static CRT (no VC++ redistributable needed).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
