#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Repository hygiene scan, run in CI (no game files involved). Fails on:
#   * anything tracked under reference/ (local Paradox dumps and raw logs must never be committed)
#   * raw CK3 engine log lines ("[hh:mm:ss][D][source:line]") in tracked files
#   * API-key shaped strings that are not obviously fake (a fake key contains FAKE)
#   * absolute home paths (/Users/<name>/)
# Usage: tools/ci/scan_patterns.sh   (scans files tracked by git; falls back to all files outside .git)
set -uo pipefail
cd "$(dirname "$0")/../.."
if git rev-parse --git-dir >/dev/null 2>&1 && [ -n "$(git ls-files | head -1)" ]; then
  list() { git ls-files; }
else
  list() { find . -type f -not -path './.git/*' -not -path './build/*' -not -path './vcpkg_installed/*' | sed 's|^\./||'; }
fi
fail=0
report() { echo "scan: $1"; fail=1; }

if list | grep -q '^reference/'; then report "files under reference/ are tracked"; fi

scan() { # name, regex, [extra grep -v pattern], [path exclude regex]
  local name="$1" regex="$2" ignore="${3:-^$}" exclude="${4:-^$}"
  local hits
  hits=$(list | grep -Ev "$exclude" | grep -Ev '\.(png|dds|ttf|otf|ico|icns)$' |
    xargs grep -I -nE -e "$regex" 2>/dev/null | grep -Ev "$ignore" |
    grep -v "tools/ci/scan_patterns.sh" || true)
  if [ -n "$hits" ]; then report "$name"; echo "$hits" | head -5 | sed 's/^/    /'; fi
}

scan "raw engine log line" '\[[0-9]{2}:[0-9]{2}:[0-9]{2}\]\[[DIEW]\]\['
scan "API-key-shaped string" 'sk-ant-[A-Za-z0-9_-]{20,}|sk-[A-Za-z0-9]{32,}|AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{35}' 'FAKE|fake'
scan "absolute home path" '/Users/[a-z][A-Za-z0-9._-]*/'

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "scan ok"
