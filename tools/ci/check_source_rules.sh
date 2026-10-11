#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Source rules for the C++ code under app/ (.h .hpp .c .cc .cpp .mm), run in CI (lint job). Fails on:
#   1. model-ID literals (case-insensitive), anywhere in app/, tests included: models are discovered from the
#      provider's models endpoint (D4); tests use neutral names such as mock-model-1.
#   2. libcurl options that can print a request (verbose, debug and trace options, which would show the
#      Authorization header) or weaken TLS (peer or host verification, NO_REVOKE). The existing
#      CURLSSLOPT_REVOKE_BEST_EFFORT is fine.
#   3. calls of reveal() on a SecretString outside the transport: only app/src/core/secret.*,
#      app/src/core/redact.*, app/src/http/, app/src/providers/anthropic_* and app/tests/ may read the bytes.
#   4. the development environment variables (CURIA_ANTHROPIC..., CURIA_MODEL...) and any getenv/getEnv of a
#      CURIA_ name outside app/src/core/dev_config.cpp and app/tests/: one reader, one place that validates.
# Comments are NOT exempt from any rule (a model ID or a variable name in a string literal is the thing to avoid,
# and telling comments from code with grep is unreliable). Rule 4 matches string literals that contain a variable
# name and getenv("CURIA_..."), so a comment that names a variable without quotes is fine.
# Usage: tools/ci/check_source_rules.sh [root-dir]   (default: the repository root; the argument is for the tests)
set -uo pipefail
if [ $# -gt 0 ]; then
  ROOT="$1"
else
  ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
fi
cd "$ROOT" 2>/dev/null || { echo "source-rules: cannot enter $ROOT" >&2; exit 2; }
# A wrong root (or a moved script) must not turn every rule into a silent no-op.
[ -d app ] || { echo "source-rules: no app/ directory under $ROOT" >&2; exit 2; }

fail=0

files() {
  find app -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.mm' \) \
    -print0 2>/dev/null
}

# check <description> <extended regex> [allowed path regex] [grep flags]
# The allowed path regex is matched against the start of "path:line:text" lines.
check() {
  local description="$1" regex="$2" allowed="${3:-^$}" flags="${4:--e}"
  local hits
  hits=$(files | xargs -0 grep -I -n -H -E "$flags" "$regex" /dev/null 2>/dev/null |
    grep -Ev "$allowed" || true)
  if [ -n "$hits" ]; then
    echo "source-rules: $description"
    echo "$hits" | head -10 | sed 's/^/    /'
    fail=1
  fi
}

check "model-ID literal in app/ (discover models from the provider; tests use mock-model-1)" \
  'claude-(fable|opus|sonnet|haiku|instant|v?[0-9])|-20[0-9]{6}([^0-9A-Za-z_]|$)|gpt-[0-9]|gpt-4o|gemini-[0-9]|llama-?[0-9]|deepseek-(chat|reasoner)|mistral-(large|small|medium)' \
  '^$' -i

check "forbidden libcurl option (verbose/debug/trace output or weakened TLS)" \
  'CURLOPT_(VERBOSE|DEBUGFUNCTION|DEBUGDATA|STDERR|TRACE_[A-Za-z0-9_]*|SSL_VERIFYPEER|SSL_VERIFYHOST|PROXY_SSL_VERIFY[A-Za-z0-9_]*)|NO_REVOKE'

check "reveal() outside the allowlist (core/secret.*, core/redact.*, http/, providers/anthropic_*, tests/)" \
  '(^|[^A-Za-z0-9_])reveal[[:space:]]*\(' \
  '^app/src/core/secret\.|^app/src/core/redact\.|^app/src/http/|^app/src/providers/anthropic_|^app/tests/'

check "CURIA_ environment variable outside app/src/core/dev_config.cpp (and app/tests/)" \
  'get[Ee]nv[[:space:]]*\([[:space:]]*"CURIA_|"[^"]*CURIA_(ANTHROPIC|MODEL)' \
  '^app/src/core/dev_config\.cpp:|^app/tests/'

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "source rules ok"
