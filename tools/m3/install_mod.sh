#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Milestone 3 helper: install (or remove) the Curia spike mod in CK3's user mod folder.
#
# Copies mod/curia from this checkout to <mod folder>/curia and writes the outer <mod folder>/curia.mod file
# (the same descriptor text plus a path= line), like the M0b/M2 install commands. The repository copy is never
# modified: every write goes to the installed copy or to the outer file.
#
# Usage: tools/m3/install_mod.sh [options]
#   --variant descriptor|metadata|both   what the installed folder carries (default both; T11 compares them):
#                                          descriptor  descriptor.mod only (no .metadata folder)
#                                          metadata    .metadata/metadata.json only (no descriptor.mod)
#                                          both        both files
#                                        The outer curia.mod file is written in every variant (the game's mod
#                                        list entry); it is a copy of the repository descriptor plus path=.
#   --supported-version <value>          rewrite supported_version in the INSTALLED copy only (descriptor.mod,
#                                        metadata.json and the outer file), for T11. Allowed characters:
#                                        letters, digits and . * + _ -    e.g. 1.19.* or 1.20.0
#   --mod-dir <dir>                      CK3 mod folder (default: ~/Documents/Paradox Interactive/Crusader Kings III/mod)
#   --dry-run                            print what would be done, change nothing
#   --uninstall                          remove <mod folder>/curia and <mod folder>/curia.mod, nothing else
#   --force                              run even if CK3 is running (it is refused otherwise)
#   -h, --help                           this text
# Exit status: 0 done, 1 failure, 2 usage error or refused (CK3 running).
# Quit CK3 completely before installing or removing: it reads mods at launch.
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/mod/curia"
DEFAULT_MOD_DIR="$HOME/Documents/Paradox Interactive/Crusader Kings III/mod"

variant=both
supported=""
mod_dir=""
dry=0
uninstall=0
force=0

say() { printf '%s\n' "$*"; }
die() { printf 'install_mod: %s\n' "$1" >&2; exit "${2:-1}"; }
usage() { sed -n '3,/^set -u$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --variant) [ $# -ge 2 ] || die "--variant needs a value" 2; variant="$2"; shift 2 ;;
    --supported-version) [ $# -ge 2 ] || die "--supported-version needs a value" 2; supported="$2"; shift 2 ;;
    --mod-dir) [ $# -ge 2 ] || die "--mod-dir needs a value" 2; mod_dir="$2"; shift 2 ;;
    --dry-run) dry=1; shift ;;
    --uninstall) uninstall=1; shift ;;
    --force) force=1; shift ;;
    -h | --help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" 2 ;;
  esac
done

case "$variant" in descriptor | metadata | both) ;; *) die "--variant must be descriptor, metadata or both" 2 ;; esac
if [ -n "$supported" ]; then
  case "$supported" in
    *[!0-9A-Za-z.*+_-]*) die "--supported-version may contain only letters, digits and . * + _ -" 2 ;;
  esac
fi

custom_mod_dir=0
if [ -n "$mod_dir" ]; then custom_mod_dir=1; else mod_dir="$DEFAULT_MOD_DIR"; fi
case "$mod_dir" in
  *'"'* | *$'\n'*) die "the mod folder path must not contain a double quote or a newline" 2 ;;
  /*) ;;
  *) mod_dir="$PWD/$mod_dir" ;;
esac
while [ "${#mod_dir}" -gt 1 ] && [ "${mod_dir%/}" != "$mod_dir" ]; do mod_dir="${mod_dir%/}"; done
case "$mod_dir/" in
  */../* | */./*) die "the mod folder path must not contain . or .. segments" 2 ;;
esac
[ "$mod_dir" != "/" ] || die "refusing to use / as the mod folder" 2
case "$mod_dir/" in
  "$REPO/mod/"* | "$SRC/"*) die "the mod folder must not be inside the repository's mod/ folder (the repository copy is never touched)" 2 ;;
esac

dest="$mod_dir/curia"
outer="$mod_dir/curia.mod"

# The folder that gets removed on install and uninstall must never be the repository or one of its
# parents (for example --mod-dir set to the folder that holds the checkout), and an existing folder
# is only removed when it looks like an install of this mod.
case "$REPO/" in
  "$dest/"*) die "refusing: $dest is the repository or one of its parent folders" 2 ;;
esac
if [ -e "$dest" ] || [ -L "$dest" ]; then
  if [ -L "$dest" ] || { [ ! -f "$dest/descriptor.mod" ] && [ ! -f "$dest/.metadata/metadata.json" ] && [ -n "$(ls -A "$dest" 2>/dev/null)" ]; }; then
    die "refusing: $dest exists but does not look like an install of this mod (no descriptor.mod or .metadata/metadata.json, or it is a symbolic link); remove it by hand if it is yours" 2
  fi
fi

ck3_running() {
  command -v pgrep >/dev/null 2>&1 || return 1
  pgrep -x ck3 >/dev/null 2>&1
}
if ck3_running; then
  if [ "$force" -eq 1 ]; then
    say "warning: CK3 is running; continuing because of --force (the game reads mods at launch, restart it)"
  else
    die "CK3 is running (process ck3). Quit it completely first, or use --force." 2
  fi
fi

run() { # <description> <command> [args...]; runs the command, or only reports it with --dry-run
  local desc="$1"
  shift
  if [ "$dry" -eq 1 ]; then
    say "would: $desc"
  else
    "$@" || die "failed: $desc"
    say "done:  $desc"
  fi
}

if [ "$uninstall" -eq 1 ]; then
  [ "$dry" -eq 1 ] && say "dry run: nothing is changed"
  say "mod folder: $mod_dir"
  found=0
  if [ -e "$dest" ] || [ -L "$dest" ]; then found=1; run "remove folder $dest" rm -rf "$dest"; fi
  if [ -e "$outer" ] || [ -L "$outer" ]; then found=1; run "remove file $outer" rm -f "$outer"; fi
  [ "$found" -eq 1 ] || say "nothing to remove (no $dest, no $outer)"
  say "Next: if the Paradox Launcher still lists Curia in the playset, remove it there."
  exit 0
fi

[ -f "$SRC/descriptor.mod" ] || die "missing $SRC/descriptor.mod (run from a complete checkout)"
[ -f "$SRC/.metadata/metadata.json" ] || die "missing $SRC/.metadata/metadata.json (run from a complete checkout)"
if [ "$custom_mod_dir" -eq 0 ] && [ ! -d "$mod_dir" ] && [ ! -d "$(dirname "$mod_dir")" ]; then
  die "CK3's user folder was not found at $(dirname "$mod_dir"); start CK3 once, or pass --mod-dir"
fi

# --- helpers that only ever write inside the installed copy or the outer file ---
remove_ds_store() { find "$dest" -name .DS_Store -type f -exec rm -f {} +; }

rewrite_supported_in_descriptor() { # <file>
  local f="$1"
  sed "s#^supported_version=.*#supported_version=\"$supported\"#" "$f" >"$f.tmp" && mv "$f.tmp" "$f" &&
    grep -q "^supported_version=\"$supported\"\$" "$f"
}
rewrite_supported_in_metadata() { # <file>
  local f="$1"
  sed -E "s#(\"supported_game_version\"[[:space:]]*:[[:space:]]*\")[^\"]*(\")#\\1$supported\\2#" "$f" >"$f.tmp" && mv "$f.tmp" "$f" &&
    grep -q "\"supported_game_version\"[[:space:]]*:[[:space:]]*\"$supported\"" "$f"
}
write_outer() {
  local text
  text="$(cat "$SRC/descriptor.mod")" || return 1
  if [ -n "$supported" ]; then
    text="$(printf '%s\n' "$text" | sed "s#^supported_version=.*#supported_version=\"$supported\"#")" || return 1
  fi
  printf '%s\npath="%s"\n' "$text" "$dest" >"$outer"
}

[ "$dry" -eq 1 ] && say "dry run: nothing is changed"
say "source:     $SRC (never modified)"
say "mod folder: $mod_dir"
say "variant:    $variant${supported:+, supported version rewritten to $supported in the installed copy}"

run "create folder $mod_dir" mkdir -p "$mod_dir"
if [ -e "$dest" ] || [ -L "$dest" ]; then run "remove the previous install $dest" rm -rf "$dest"; fi
run "copy mod/curia to $dest" cp -R "$SRC" "$dest"
run "drop .DS_Store files from the installed copy" remove_ds_store
case "$variant" in
  descriptor) run "remove .metadata from the installed copy (variant descriptor)" rm -rf "$dest/.metadata" ;;
  metadata) run "remove descriptor.mod from the installed copy (variant metadata)" rm -f "$dest/descriptor.mod" ;;
esac
if [ -n "$supported" ]; then
  if [ "$variant" != "metadata" ]; then
    run "set supported_version=\"$supported\" in the installed descriptor.mod" rewrite_supported_in_descriptor "$dest/descriptor.mod"
  fi
  if [ "$variant" != "descriptor" ]; then
    run "set supported_game_version \"$supported\" in the installed metadata.json" rewrite_supported_in_metadata "$dest/.metadata/metadata.json"
  fi
fi
run "write the outer file $outer (path=\"$dest\")" write_outer

if [ "$dry" -eq 0 ]; then
  say ""
  say "installed files (relative to $mod_dir):"
  (cd "$mod_dir" && { find curia -type f | sort; echo curia.mod; }) | sed 's/^/  /'
  say ""
  say "outer file:"
  sed 's/^/  /' "$outer"
fi
if [ "$dry" -eq 0 ]; then
  say ""
  say "Next: open the Paradox Launcher, make sure 'Curia' is in the active playset and enabled, then Play (normal launch,"
  say "never debug mode). Start a NEW game: the load line is expected at the start of a new game (whether a loaded save prints it is not known)."
fi
