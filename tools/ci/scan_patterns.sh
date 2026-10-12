#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Repository hygiene scan, run in CI (no game files involved). Fails on:
#   * anything tracked under reference/ (local Paradox dumps and raw logs must never be committed)
#   * raw CK3 engine log lines ("[hh:mm:ss][D][source:line]") in tracked files
#   * API-key shaped tokens (Anthropic, OpenAI-style, AWS, Google) and real-looking Anthropic request ids
#     (msg_01..., req_01..., toolu_01...). The check is per token: a matched token is exempt only if the
#     token itself contains FAKE or fake, so a real key on a line that mentions "fake" is still caught.
#     Synthetic ids such as msg_mock0001 do not match the shape at all.
#   * a literal "x-api-key:" or "Authorization: Bearer" header followed by a token of 20 or more characters,
#     outside docs/ (same per-token FAKE exemption)
#   * a tracked path with a path segment named transcripts or curia_m4_results (dev content transcripts live
#     outside the repository; the segment names are also in .gitignore)
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

# Like scan, but matches are extracted with grep -o and judged one by one: a token that contains FAKE or fake
# is dropped, everything else is a hit. (A line-level "contains fake" exemption would let a real key through
# on any line that happens to say fake.)
scan_tokens() { # name, regex, [path exclude regex], [extra grep flags, e.g. -i]
  local name="$1" regex="$2" exclude="${3:-^$}" flags="${4:--e}"
  local hits
  hits=$(list | grep -Ev "$exclude" | grep -Ev '\.(png|dds|ttf|otf|ico|icns)$' | tr '\n' '\0' |
    xargs -0 grep -I -o -n -H -E "$flags" "$regex" /dev/null 2>/dev/null |
    grep -v "tools/ci/scan_patterns.sh" |
    awk '{ token = $0; sub(/^[^:]*:[0-9]+:/, "", token); if (token !~ /FAKE|fake/) print $0 }' || true)
  if [ -n "$hits" ]; then report "$name"; echo "$hits" | head -5 | sed 's/^/    /'; fi
}

scan "raw engine log line" '\[[0-9]{2}:[0-9]{2}:[0-9]{2}\]\[[DIEW]\]\['
scan_tokens "API-key-shaped string" 'sk-ant-[A-Za-z0-9_-]{20,}|sk-[A-Za-z0-9]{32,}|AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{35}'
scan_tokens "Anthropic request id shape (msg_01/req_01/toolu_01)" '(msg|req|toolu)_01[A-Za-z0-9]{16,}'
scan_tokens "API-key header with a literal token" \
  'x-api-key"?:[[:space:]]*"?[A-Za-z0-9_.~+/=-]{20,}|authorization"?:[[:space:]]*"?bearer[[:space:]]+[A-Za-z0-9_.~+/=-]{20,}' \
  '^docs/' -i
scan "absolute home path" '/Users/[a-z][A-Za-z0-9._-]*/'

if list | grep -Eq '(^|/)(transcripts|curia_m4_results)(/|$)'; then
  report "a tracked path has a transcripts or curia_m4_results segment (dev transcripts must stay outside the repository)"
  list | grep -E '(^|/)(transcripts|curia_m4_results)(/|$)' | head -5 | sed 's/^/    /'
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "scan ok"
