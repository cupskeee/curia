#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Release blockers (M7 criterion 11, owner decision 2026-10-11): the spike and development parts that must be gone
# before a release. Lists every one still present:
#   * the mod's "Curia probes" HUD button and the probe code: curia_probes_button in mod/curia/gui/curia_widget.gui,
#     the scripted GUI curia_run_probes, curia_probes.txt, the curia_probe_* localization keys and the
#     curia_probe_mode variable;
#   * mod/curia-m0 (the M0 stub mod) and mod/curia-m2-probe (the M2 stub mod);
#   * app/probe/ (the M2 overlay probe), its tests app/tests/test_probe_*.cpp, and the CMake targets curia_m2_probe
#     and curia_probe_core;
#   * a release configuration that leaves CURIA_ENABLE_DEV_ENV on (D19): until a configure preset in
#     CMakePresets.json whose name contains "release" sets it OFF in its own cacheVariables, this counts as a blocker.
# Default: print the list and exit 0 (a warning, M4 to M6; the lint job runs it so). --enforce: exit 1 if any remain
# (the M7 release workflow). Exit 2 on a usage error or a root without app/ and mod/.
# Usage: tools/ci/release_blockers.sh [--enforce] [root-dir]   (default root: the repository; the argument is for tests)
set -uo pipefail

enforce=0
root=""
for arg in "$@"; do
  case "$arg" in
    --enforce) enforce=1 ;;
    -*) echo "usage: tools/ci/release_blockers.sh [--enforce] [root-dir]" >&2; exit 2 ;;
    *)
      if [ -n "$root" ]; then echo "usage: tools/ci/release_blockers.sh [--enforce] [root-dir]" >&2; exit 2; fi
      root="$arg"
      ;;
  esac
done
if [ -z "$root" ]; then
  root="$(cd "$(dirname "$0")/../.." && pwd)"
fi
cd "$root" 2>/dev/null || { echo "release-blockers: cannot enter $root" >&2; exit 2; }
# A wrong root (or a moved script) must not report "none".
if [ ! -d app ] || [ ! -d mod ]; then
  echo "release-blockers: no app/ and mod/ under $root" >&2
  exit 2
fi

blockers=()
blocker() { blockers+=("$1"); }

# The mod's probe button and probe code.
if grep -qs 'curia_probes_button' mod/curia/gui/curia_widget.gui; then
  blocker 'the "Curia probes" HUD button (curia_probes_button in mod/curia/gui/curia_widget.gui)'
fi
if grep -rqs 'curia_run_probes' mod/curia; then
  blocker 'the scripted GUI curia_run_probes (mod/curia)'
fi
probes_file="$(find mod/curia -type f -name 'curia_probes.txt' 2>/dev/null | head -1)"
if [ -n "$probes_file" ]; then
  blocker "the probe effects $probes_file"
fi
if grep -rqsE '^[[:space:]]*curia_probe_[A-Za-z0-9_]*:' mod/curia/localization; then
  blocker 'the curia_probe_* localization keys (mod/curia/localization)'
fi
if grep -rqs 'curia_probe_mode' mod/curia; then
  blocker 'the curia_probe_mode variable (mod/curia)'
fi

# The stub mods and the overlay probe.
if [ -e mod/curia-m0 ]; then blocker 'mod/curia-m0 (the M0 stub mod)'; fi
if [ -e mod/curia-m2-probe ]; then blocker 'mod/curia-m2-probe (the M2 stub mod)'; fi
if [ -e app/probe ]; then blocker 'app/probe/ (the M2 overlay probe)'; fi
probe_tests="$(find app/tests -maxdepth 1 -type f -name 'test_probe_*' 2>/dev/null | sort | tr '\n' ' ')"
if [ -n "$probe_tests" ]; then
  blocker "the probe's tests: ${probe_tests% }"
fi
for target in curia_m2_probe curia_probe_core; do
  # Not under build trees or hidden folders (such as .git, tool caches, virtual environments).
  if find . -name CMakeLists.txt -not -path './build*' -not -path './.*' -print0 2>/dev/null |
    xargs -0 grep -qsw "$target" /dev/null; then
    blocker "the CMake target $target"
  fi
done

# The release configuration must turn the development configuration off (D19).
release_off="no"
if [ -f CMakePresets.json ]; then
  release_off="$(python3 - CMakePresets.json <<'PY' 2>/dev/null || echo "unreadable"
import json, sys
try:
    presets = json.load(open(sys.argv[1], encoding="utf-8")).get("configurePresets", [])
except (OSError, ValueError):
    print("unreadable"); sys.exit(0)
for preset in presets:
    value = str((preset.get("cacheVariables") or {}).get("CURIA_ENABLE_DEV_ENV", "")).upper()
    if "release" in str(preset.get("name", "")).lower() and value in ("OFF", "FALSE", "0", "NO"):
        print("yes"); break
else:
    print("no")
PY
)"
fi
if [ "$release_off" != "yes" ]; then
  blocker 'no release configuration turns CURIA_ENABLE_DEV_ENV off (a configure preset named *release* in CMakePresets.json with CURIA_ENABLE_DEV_ENV OFF)'
fi

if [ "${#blockers[@]}" -eq 0 ]; then
  echo "release blockers: none"
  exit 0
fi
echo "release blockers: ${#blockers[@]} still present (remove them before a release, M7 criterion 11):"
for item in "${blockers[@]}"; do
  echo "  - $item"
done
if [ "$enforce" -eq 1 ]; then
  exit 1
fi
echo "release blockers: warning only (--enforce makes this fail)"
exit 0
