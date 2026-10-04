#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fails if any C++ source is not formatted with the pinned clang-format (tools/requirements-dev.txt).
# Usage: tools/ci/check_format.sh [--fix]
set -euo pipefail
cd "$(dirname "$0")/../.."
FMT="${CLANG_FORMAT:-clang-format}"
want="$(sed -n 's/^clang-format==//p' tools/requirements-dev.txt)"
have="$("$FMT" --version | sed -n 's/.*version \([0-9.]*\).*/\1/p')"
if [ "${have%%.*}" != "${want%%.*}" ]; then
  echo "warning: clang-format $have found, pinned major is ${want%%.*} (pip install -r tools/requirements-dev.txt)" >&2
fi
files=$(find app -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) -not -path '*/build/*' | sort)
count=$(printf '%s\n' "$files" | grep -c .)
if [ "${1:-}" = "--fix" ]; then
  printf '%s\n' "$files" | xargs "$FMT" -i
  echo "formatted $count files"
else
  printf '%s\n' "$files" | xargs "$FMT" --dry-run -Werror
  echo "format ok ($count files)"
fi
