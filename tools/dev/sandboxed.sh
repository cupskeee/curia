#!/bin/sh
# SPDX-License-Identifier: MIT
# Runs a command with HOME set to a fresh, empty temporary folder, which is removed afterwards.
#
#   tools/dev/sandboxed.sh ctest --test-dir build/x -L "unit|e2e|fuzz" -j 8
#   tools/dev/sandboxed.sh python3 -I -m unittest discover -s tools/m4
#
# Tests and tools are run this way, never the build (owner decision D26, 2026-10-19): a default
# path in the app, a test or a tool then cannot reach the real home folder (Curia's app log in
# ~/Library/Logs/Curia, CK3's folder in ~/Documents/Paradox Interactive, the results folders),
# even by mistake. ctest also gives every test a home folder of its own (app/CMakeLists.txt).
#
# The variables that name other folders under the real home (XDG_*, ZDOTDIR) are unset. pyenv's
# shims find their Pythons through HOME, so PYENV_ROOT is kept pointing at the real pyenv folder.
set -eu

if [ "$#" -eq 0 ]; then
    echo "usage: tools/dev/sandboxed.sh <command> [arguments...]" >&2
    exit 2
fi

if [ -z "${PYENV_ROOT:-}" ] && [ -d "${HOME:-/nonexistent}/.pyenv" ]; then
    PYENV_ROOT="$HOME/.pyenv"
    export PYENV_ROOT
fi

temp_base=${TMPDIR:-/tmp}
sandbox_home=$(mktemp -d "${temp_base%/}/curia-home.XXXXXX")
trap 'rm -rf "$sandbox_home"' EXIT INT TERM
unset XDG_CONFIG_HOME XDG_DATA_HOME XDG_STATE_HOME XDG_CACHE_HOME ZDOTDIR
HOME="$sandbox_home"
export HOME

status=0
"$@" || status=$?
exit "$status"
