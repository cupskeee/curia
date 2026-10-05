#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Milestone 2 helper (probe T7): collect the CURIAPROBE1 lines and sizes from CK3's logs (macOS).
# Usage: tools/m2/check_t7.sh [label]      e.g. tools/m2/check_t7.sh unpaused
# Read-only for the game folders. Saves a copy of the output under ~/curia_m2_results/ (CK3 rewrites
# debug.log at every launch, so run it while the game is open or right after quitting).
# Overrides for testing: CURIA_LOGDIR (log folder), CURIA_RESULTS_DIR (where the copy is saved).
set -u
LABEL="${1:-X}"
LOGDIR="${CURIA_LOGDIR:-$HOME/Documents/Paradox Interactive/Crusader Kings III/logs}"
OUTDIR="${CURIA_RESULTS_DIR:-$HOME/curia_m2_results}"
mkdir -p "$OUTDIR"
OUT="$OUTDIR/t7-$LABEL-$(date +%Y%m%d-%H%M%S).txt"
DEBUG="$LOGDIR/debug.log"
ERRORS="$LOGDIR/error.log"
MARK='CURIAPROBE1|'

count() { local c; c=$(grep -a -c "$1" "$DEBUG" 2>/dev/null || true); echo "${c:-0}"; }
size() { wc -c < "$1" 2>/dev/null | tr -d ' '; }
mtime() { /usr/bin/stat -f '%Sm' "$1" 2>/dev/null || stat -c '%y' "$1" 2>/dev/null; }

{
  echo "== Curia M2 T7 check, label $LABEL, $(date '+%Y-%m-%d %H:%M:%S')"
  echo "== log folder: $LOGDIR"
  echo
  echo "== 1. Did the probe mod load?"
  if [ -f "$DEBUG" ]; then
    echo "-- load lines (CURIAPROBE1|T7|load|), with line numbers:"
    grep -a -n 'CURIAPROBE1|T7|load|' "$DEBUG" | cut -c1-260
    if ! grep -a -q 'CURIAPROBE1|T7|load|' "$DEBUG"; then
      echo "(none found: the load line is written only when a NEW game starts; if you loaded an existing save, or the mod is not enabled in the playset, it is absent)"
    fi
    echo "-- other debug.log lines mentioning the mod (excluding probe lines), first 10:"
    grep -a -n -i -E 'curia_m2|curia m2' "$DEBUG" | grep -a -v "$MARK" | head -10 | cut -c1-260
    if ! grep -a -i -E 'curia_m2|curia m2' "$DEBUG" | grep -a -q -v "$MARK"; then
      echo "(none)"
    fi
  else
    echo "debug.log not found at: $DEBUG"
  fi
  echo
  echo "== 2. All CURIAPROBE1 lines in order (line number first, cut to 260 characters; the line after a probe line is the DATE line)"
  grep -a -n -A1 "$MARK" "$DEBUG" 2>/dev/null | cut -c1-260
  grep -a "$MARK" "$DEBUG" 2>/dev/null > "$OUTDIR/t7-raw-$LABEL-$(date +%Y%m%d-%H%M%S).txt"
  echo "(full lines saved to $OUTDIR/t7-raw-$LABEL-*.txt)"
  echo
  echo "== 3. Counts"
  echo "click lines: $(count 'CURIAPROBE1|T7|click|')"
  echo "load lines: $(count 'CURIAPROBE1|T7|load|')"
  echo "all CURIAPROBE1 lines: $(count "$MARK")"
  echo
  echo "== 4. error.log lines mentioning the mod (curia_m2 or the mod name); please report all of them"
  if [ -f "$ERRORS" ]; then
    grep -a -n -i -E 'curia_m2|curia m2' "$ERRORS" | cut -c1-260 | head -40
    if ! grep -a -q -i -E 'curia_m2|curia m2' "$ERRORS"; then
      echo "(none)"
    fi
  else
    echo "error.log not found at: $ERRORS"
  fi
  echo
  echo "== 5. Sizes"
  echo "probe line bytes (all CURIAPROBE1 lines): $(grep -a "$MARK" "$DEBUG" 2>/dev/null | wc -c | tr -d ' ')"
  for n in debug.log error.log game.log; do
    if [ -f "$LOGDIR/$n" ]; then
      echo "$n: $(size "$LOGDIR/$n") bytes, modified $(mtime "$LOGDIR/$n")"
    else
      echo "$n: not found"
    fi
  done
} 2>&1 | tee "$OUT"

echo
echo "Saved to: $OUT"
