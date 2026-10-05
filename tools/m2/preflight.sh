#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Milestone 2 preflight: run before the session, after every rebuild of the probe.
# Opens no window and changes no focus: (1) the workspace-observer self-test, (2) a one-second
# `detect` run launched with `open` (no CK3 window is queried in that time) that must end cleanly,
# (3) no new crash report for the probe may appear. Prints PREFLIGHT_OK or PREFLIGHT_FAIL.
# Usage: tools/m2/preflight.sh [path/to/curia_m2_probe.app]
set -u
cd "$(dirname "$0")/../.." || exit 1
APP="${1:-$PWD/build/macos-arm64/app/curia_m2_probe.app}"
BIN="$APP/Contents/MacOS/curia_m2_probe"
REPORTS="$HOME/Library/Logs/DiagnosticReports"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/curia_m2_preflight.XXXXXX")"
fail() { echo "PREFLIGHT_FAIL: $1"; exit 1; }

[ -x "$BIN" ] || fail "probe not built at $APP (see the checklist, section 1)"

before=$(find "$REPORTS" -maxdepth 1 -name 'curia_m2_probe*' 2>/dev/null | wc -l | tr -d ' ')

echo "1. observer self-test"
"$BIN" --selftest-observers | tee "$OUT/selftest.txt"
grep -q PROBE_SELFTEST_OK "$OUT/selftest.txt" || fail "observer self-test"

echo "2. one-second detect run (no window, no sound)"
open -g -n "$APP" --args --stage detect --seconds 1 --start-delay 0 --no-sound --results-dir "$OUT"
i=0
while pgrep -f "$BIN" >/dev/null 2>&1; do
  i=$((i + 1)); [ "$i" -gt 60 ] && fail "the detect run did not finish within 15 s"; sleep 0.25
done
log=$(find "$OUT" -name 'probe-detect-*.log' 2>/dev/null | head -1)
[ -n "$log" ] || fail "no log file was written"
grep -q ' end t=' "$log" || fail "the log has no end line (the probe did not exit cleanly): $log"

echo "3. crash reports"
sleep 3  # the system writes a crash report a moment after the crash
after=$(find "$REPORTS" -maxdepth 1 -name 'curia_m2_probe*' 2>/dev/null | wc -l | tr -d ' ')
[ "$after" -eq "$before" ] || fail "a new crash report appeared in $REPORTS"

echo "PREFLIGHT_OK (log: $log)"
