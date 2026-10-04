#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Milestone 0 helper: collect the CURIA_M0 test lines and sizes from CK3's logs (macOS).
# Usage: tools/m0/check.sh <run-letter>      e.g. tools/m0/check.sh A
# Read-only for the game folders. Saves a copy of the output under ~/curia_m0_results/ because
# CK3 rewrites debug.log at every launch. Run it while the game is open or right after quitting,
# before the next launch.
set -u
RUN="${1:-X}"
LOGDIR="$HOME/Documents/Paradox Interactive/Crusader Kings III/logs"
OUTDIR="$HOME/curia_m0_results"
mkdir -p "$OUTDIR"
OUT="$OUTDIR/run-$RUN-$(date +%Y%m%d-%H%M%S).txt"

{
  echo "== Curia M0 check, run $RUN, $(date '+%Y-%m-%d %H:%M:%S')"
  echo "== macOS $(sw_vers -productVersion), launcher game version: $(grep -o '"rawVersion": *"[^"]*"' "$HOME/Library/Application Support/Steam/steamapps/common/Crusader Kings III/launcher/launcher-settings.json" 2>/dev/null)"
  echo
  echo "== 1. Did the stub mod load? (lines that mention the mod, excluding test lines)"
  if [ -f "$LOGDIR/debug.log" ]; then
    grep -a -i -E 'curia_m0|curia m0' "$LOGDIR/debug.log" | grep -a -v 'CURIA_M0|' | head -5
    if ! grep -a -i -E 'curia_m0|curia m0' "$LOGDIR/debug.log" | grep -a -q -v 'CURIA_M0|'; then
      echo "(none found: if this stays empty the mod probably did NOT load; a run without it is invalid, not a T1 failure)"
    fi
  else
    echo "debug.log not found at: $LOGDIR/debug.log"
  fi
  echo
  echo "== 2. Test lines in debug.log (shown cut to 260 characters; each followed by the next log line, which is the date after the first line)"
  grep -a -n -A1 'CURIA_M0|' "$LOGDIR/debug.log" 2>/dev/null | cut -c1-260
  echo
  echo "== 2b. Full length in characters of each test line (shows whether long lines were cut), and whether the _END tag survived"
  grep -a 'CURIA_M0|' "$LOGDIR/debug.log" 2>/dev/null | awk '{ n=match($0,/CURIA_M0\|[A-Z]+[0-9]+\|/); id=substr($0,n,RLENGTH); printf "%s total=%d end_tag=%s\n", id, length($0), ($0 ~ /_END$/ ? "yes" : "no") }'
  grep -a 'CURIA_M0|' "$LOGDIR/debug.log" 2>/dev/null > "$OUTDIR/raw-$RUN-$(date +%Y%m%d-%H%M%S).txt"
  echo "(full lines saved to $OUTDIR/raw-$RUN-*.txt)"
  echo
  echo "== 3. Where else do test lines appear? (count per log file)"
  for f in "$LOGDIR"/*.log; do
    c=$(grep -a -c 'CURIA_M0' "$f" 2>/dev/null || true)
    [ "${c:-0}" != "0" ] && echo "$(basename "$f"): $c"
  done
  echo "(only debug.log is expected; anything else is worth reporting)"
  echo
  echo "== 4. Errors mentioning the mod in error.log"
  grep -a -i -B1 -A3 -E 'curia_m0|curia m0' "$LOGDIR/error.log" 2>/dev/null | head -30
  echo
  echo "== 4b. error.log tail (last 60 lines, cut to 220 characters; it is recreated at each launch)"
  tail -n 60 "$LOGDIR/error.log" 2>/dev/null | cut -c1-220
  echo
  echo "== 5. Sizes in bytes"
  echo "marker bytes (all CURIA_M0 lines): $(grep -a 'CURIA_M0|' "$LOGDIR/debug.log" 2>/dev/null | wc -c | tr -d ' ')"
  echo "marker lines: $(grep -a -c 'CURIA_M0|' "$LOGDIR/debug.log" 2>/dev/null)"
  for n in debug.log error.log game.log; do
    echo "$n: $(/usr/bin/stat -f %z "$LOGDIR/$n" 2>/dev/null) bytes, modified $(/usr/bin/stat -f '%Sm' "$LOGDIR/$n" 2>/dev/null)"
  done
} 2>&1 | tee "$OUT"

echo
echo "Saved to: $OUT"
