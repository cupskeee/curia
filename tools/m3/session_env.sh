# SPDX-License-Identifier: MIT
# shellcheck shell=bash
# Source this file in the Terminal tab used for commands (tab 2) during the M3 sessions:
#     cd ~/PycharmProjects/curia && source tools/m3/session_env.sh
# It sets the folder variables and defines the helper functions of docs/spikes/m3-protocol.md section 0.1.
# Works in bash and zsh. It only reads the game's logs and the mod folder and writes small files under
# ~/curia_m3_results/. CK3, GAME and RESULTS can be set before sourcing (for tests with a temporary folder).

CK3="${CK3:-$HOME/Documents/Paradox Interactive/Crusader Kings III}"
LOGS="$CK3/logs"
MODDIR="$CK3/mod"
GAME="${GAME:-$HOME/Library/Application Support/Steam/steamapps/common/Crusader Kings III}"
RESULTS="${RESULTS:-$HOME/curia_m3_results}"
mkdir -p "$RESULTS"

# t11_snap <tag>: a checksum list of the mod folder.
t11_snap() {
  find "$MODDIR" -type f ! -name .DS_Store -exec shasum -a 256 {} + | sort -k2 > "$RESULTS/t11-$1.sha.txt"
}

# errcount / errlines: the error.log check of the stop rules (case-insensitive curia[_/]).
errcount() { grep -a -i -c -E 'curia[_/]' "$LOGS/error.log"; }
errlines() { grep -a -i -n -E 'curia[_/]' "$LOGS/error.log" | cut -c1-260; }

# t11_check <tag>: counts (not text) from the logs, printed and saved.
t11_check() {
  {
    echo "== T11 check, label $1, $(date '+%Y-%m-%d %H:%M:%S')"
    echo "LOAD lines in debug.log (CURIA1|0|LOAD|): $(grep -a -c 'CURIA1|0|LOAD|' "$LOGS/debug.log")"
    echo "other debug.log lines mentioning curia (CURIA1 lines excluded; a wide match, vanilla words can count): $(grep -a -i curia "$LOGS/debug.log" | grep -a -v -c 'CURIA1|')"
    echo "error.log lines matching curia[_/]: $(errcount)"
    echo "debug.log: $(stat -f '%z bytes, inode %i' "$LOGS/debug.log"); error.log: $(stat -f '%z bytes' "$LOGS/error.log")"
  } 2>&1 | tee "$RESULTS/t11-$1-$(date +%Y%m%d-%H%M%S).txt"
}
