#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The project version lives in CMakeLists.txt (project(VERSION)); vcpkg.json must match, and
# CHANGELOG.md must have an [Unreleased] section. Release tags are checked against this in release.yml.
set -euo pipefail
cd "$(dirname "$0")/../.."
cmake_version="$(sed -n 's/^project(curia VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)"
vcpkg_version="$(jq -r '."version-semver"' vcpkg.json)"
baseline="$(jq -r '."builtin-baseline"' vcpkg.json)"
status=0
if [ -z "$cmake_version" ]; then echo "check_versions: cannot read the version from CMakeLists.txt"; status=1; fi
if [ "$cmake_version" != "$vcpkg_version" ]; then
  echo "check_versions: CMake version '$cmake_version' != vcpkg.json version-semver '$vcpkg_version'"; status=1
fi
if ! printf '%s' "$baseline" | grep -Eq '^[0-9a-f]{40}$'; then
  echo "check_versions: vcpkg.json builtin-baseline is not a 40-character commit hash"; status=1
fi
if ! grep -q '^## \[Unreleased\]' CHANGELOG.md; then
  echo "check_versions: CHANGELOG.md has no [Unreleased] section"; status=1
fi
[ "$status" -eq 0 ] && echo "versions ok ($cmake_version, vcpkg baseline ${baseline:0:10})"
exit "$status"
