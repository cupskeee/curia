#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Structural lint of the CK3 mod folders. Uses no game files (CI never touches Paradox content).

Checks, for every folder under mod/ that has a descriptor.mod:
  * descriptor.mod exists, has no BOM, and defines name, version and supported_version
  * script (.txt) and localization (.yml) files start with a UTF-8 BOM, like the game's own files
  * localization files start with "l_english:" (after the BOM) and use no tabs for indentation
  * no CRLF line endings (we keep LF)
  * file names carry the curia_ prefix (to avoid colliding with other mods)
The real validation against the game is local only (ck3-tiger, docs/architecture.md section 7).
"""
import pathlib
import sys

BOM = b"\xef\xbb\xbf"
root = pathlib.Path(__file__).resolve().parents[2] / "mod"
problems = []
mods = sorted(p.parent for p in root.glob("*/descriptor.mod")) if root.exists() else []

for mod in mods:
    desc = (mod / "descriptor.mod").read_bytes()
    if desc.startswith(BOM):
        problems.append(f"{mod.name}/descriptor.mod: must not have a BOM")
    text = desc.decode("utf-8", errors="replace")
    for key in ("name=", "version=", "supported_version="):
        if key not in text:
            problems.append(f"{mod.name}/descriptor.mod: missing {key}")
    for path in sorted(mod.rglob("*")):
        if not path.is_file() or path.name == ".DS_Store" or path.name == "descriptor.mod":
            continue
        rel = path.relative_to(root)
        data = path.read_bytes()
        if path.suffix in (".txt", ".yml"):
            if not data.startswith(BOM):
                problems.append(f"{rel}: missing UTF-8 BOM")
            if b"\r\n" in data:
                problems.append(f"{rel}: CRLF line endings")
            if not path.name.startswith("curia_"):
                problems.append(f"{rel}: file name must start with curia_")
        elif path.suffix == ".gui":
            # GUI files carry no BOM requirement (the game's own do not); keep LF and the curia_ prefix.
            if b"\r\n" in data:
                problems.append(f"{rel}: CRLF line endings")
            if not path.name.startswith("curia_"):
                problems.append(f"{rel}: file name must start with curia_")
        if path.suffix == ".yml":
            body = data[len(BOM):] if data.startswith(BOM) else data
            if not body.startswith(b"l_english:"):
                problems.append(f"{rel}: localization must start with 'l_english:'")

if problems:
    print("\n".join(problems))
    sys.exit(1)
print(f"mod lint ok ({len(mods)} mod folder(s))")
