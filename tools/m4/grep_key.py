#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Looks for the owner's API key in files, without ever showing it (criterion 6 of M4).

The key is read from the environment variable CURIA_ANTHROPIC_API_KEY of the shell that runs this
tool, the same one the sitting exported it in (docs/spikes/m4-session-sheet.md), so it never appears
on a command line or in a file. The tool scans the files and folders given on the command line for
the key's bytes as they are, JSON-escaped, URL-encoded, in base64 (each of the three alignments, both
alphabets) and in UTF-16, for a piece of it (its first 24 characters as they are: a key cut by a
length cap, say), and counts separately the generic shape "sk-ant-..." (which may also be another
key, or a test key that says FAKE). Not found: the key inside a compressed file (.gz, .zip) and in
base64 that is split over lines (as in mail or PEM files).

    read -s "CURIA_ANTHROPIC_API_KEY?API key (nothing is shown): "; export CURIA_ANTHROPIC_API_KEY
    python3 -I tools/m4/grep_key.py ~/curia_m4_results ~/Library/Logs/Curia
    python3 -I tools/m4/grep_key.py ~/Library/Logs/DiagnosticReports --name-contains curia

It prints only paths and counts: never the key, a part of it, its length or the text around a match.
A path that itself contains the key is not printed either, and the home folder is printed as "~".

Folders are scanned recursively. Symbolic links inside them are not followed (counted as skipped), so
the scan stays inside the folders given; a path given on the command line is followed even if it is
a link. Special files (pipes, devices, sockets) are skipped. Each file is read in pieces, binary-safe,
up to --max-bytes (default 256 MiB); a file that is larger is reported as partly scanned.

Exit status: 0 the key was found nowhere (paths that do not exist are listed and skipped); 1 it was
found (the files are listed); 2 usage error: the variable is not set or shorter than 8 characters,
the key was typed on the command line, none of the paths exists, or an internal error; 3 the key was
not found in what could be read, but a file or folder could not be read or a file was read only in
part (listed): not yet a clean result.

Standard library only, Python 3.10 or later; meant to run with `python3 -I`.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import re
import stat
import sys
import urllib.parse
from typing import Callable, Dict, List, NoReturn, Optional, Sequence, Tuple

VARIABLE = "CURIA_ANTHROPIC_API_KEY"
MIN_KEY_CHARS = 8
DEFAULT_MAX_BYTES = 256 * 1024 * 1024
CHUNK_BYTES = 1024 * 1024
SHAPE_RE = re.compile(rb"sk-ant-[A-Za-z0-9_\-]{8,}")
# A piece of the key: its first characters, longer than any shared prefix of a key's shape. Looked
# for only when the key is longer still (otherwise the piece is most of the key itself).
PIECE_CHARS = 24
PIECE_NAME = "a piece of the key (its first %d characters)" % PIECE_CHARS
# A shape match is counted where it starts; pieces overlap by this much so that none is cut.
SHAPE_OVERLAP = 512


def _base64_pieces(data: bytes) -> List[bytes]:
    """The base64 text that `data` produces wherever it sits in a longer base64 stream: one piece
    per alignment (0, 1 or 2 bytes before it), without the characters that also depend on the bytes
    around it."""
    pieces = []
    for shift, skip in ((0, 0), (1, 2), (2, 3)):
        encoded = base64.b64encode(b"\0" * shift + data)
        whole = (8 * (shift + len(data))) // 6  # characters made from these bytes only
        piece = encoded[skip:whole]
        if len(piece) >= MIN_KEY_CHARS:
            pieces.append(piece)
    return pieces


def encodings(key: str) -> Dict[bytes, List[str]]:
    """The byte strings to look for, each with the names of the encodings that give it."""
    raw = key.encode("utf-8")
    forms: List[Tuple[str, bytes]] = [("raw", raw)]
    escaped = json.dumps(key)[1:-1]
    forms.append(("JSON-escaped", escaped.encode("ascii")))
    forms.append(("JSON-escaped", escaped.replace("/", "\\/").encode("ascii")))
    forms.append(("URL-encoded", urllib.parse.quote(key, safe="").encode("ascii")))
    forms.append(("URL-encoded", urllib.parse.quote_plus(key, safe="").encode("ascii")))
    for piece in _base64_pieces(raw):
        forms.append(("base64", piece))
        forms.append(("base64", piece.replace(b"+", b"-").replace(b"/", b"_")))
    forms.append(("UTF-16", key.encode("utf-16-le")))
    forms.append(("UTF-16", key.encode("utf-16-be")))
    needles: Dict[bytes, List[str]] = {}
    for name, needle in forms:
        names = needles.setdefault(needle, [])
        if name not in names:
            names.append(name)
    return needles


def key_piece(key: str) -> Optional[bytes]:
    raw = key.encode("utf-8")
    return raw[:PIECE_CHARS] if len(raw) >= PIECE_CHARS + MIN_KEY_CHARS else None


class FileResult:
    def __init__(self) -> None:
        self.key: Dict[str, int] = {}
        self.shapes = 0
        self.fake_shapes = 0
        self.scanned = 0
        self.size = 0
        self.partial = False


def count_in(buffer: bytes, needle: bytes, limit: int) -> int:
    """Occurrences of `needle` in `buffer` that start before `limit`."""
    count = 0
    at = buffer.find(needle)
    while 0 <= at < limit:
        count += 1
        at = buffer.find(needle, at + len(needle))
    return count


def count_pieces(buffer: bytes, piece: bytes, wholes: Sequence[bytes], limit: int) -> int:
    """Occurrences of `piece` that start before `limit` and are not the start of a whole form."""
    count = 0
    at = buffer.find(piece)
    while 0 <= at < limit:
        if not any(buffer.startswith(whole, at) for whole in wholes):
            count += 1
        at = buffer.find(piece, at + len(piece))
    return count


def scan_file(path: str, needles: Dict[bytes, List[str]], key_bytes: Sequence[bytes], max_bytes: int,
              chunk_bytes: int = CHUNK_BYTES, piece: Optional[bytes] = None) -> FileResult:
    """Counts the key's forms, its piece and the shapes in one file. Every start position is looked
    at in exactly one piece of the file: a piece is the carry of the last one plus new bytes, and
    only matches that start before its last `overlap` bytes are counted, unless it is the last."""
    result = FileResult()
    overlap = max([SHAPE_OVERLAP] + [len(n) for n in needles])
    wholes = [n for n in needles if piece is not None and n.startswith(piece)]
    with open(path, "rb") as handle:
        try:
            result.size = os.fstat(handle.fileno()).st_size
        except OSError:
            result.size = 0
        carry = b""
        while True:
            want = min(chunk_bytes, max_bytes - result.scanned)
            chunk = handle.read(want) if want > 0 else b""
            result.scanned += len(chunk)
            last = len(chunk) < want or want <= 0 or result.scanned >= max_bytes
            if last and result.scanned >= max_bytes and handle.read(1):
                result.partial = True
            buffer = carry + chunk
            limit = len(buffer) if last else max(0, len(buffer) - overlap)
            for needle, names in needles.items():
                found = count_in(buffer, needle, limit)
                if found:
                    # Encodings that give the same bytes are one match: "raw (also JSON-escaped)".
                    name = names[0] if len(names) == 1 else "%s (also %s)" % (names[0], ", ".join(names[1:]))
                    result.key[name] = result.key.get(name, 0) + found
            if piece is not None:
                found = count_pieces(buffer, piece, wholes, limit)
                if found:
                    result.key[PIECE_NAME] = result.key.get(PIECE_NAME, 0) + found
            for match in SHAPE_RE.finditer(buffer):
                if match.start() >= limit:
                    break
                text = match.group(0)
                if any(k in text for k in key_bytes) or (piece is not None and piece in text):
                    continue  # the key itself or its piece, counted above
                result.shapes += 1
                if b"FAKE" in text:
                    result.fake_shapes += 1
            if last:
                break
            carry = buffer[limit:]
    return result


def absent(path: str) -> bool:
    """Whether the path does not exist; a path that cannot be looked at (a folder on the way that
    cannot be searched) may exist, so it is not absent."""
    try:
        os.lstat(path)
    except (FileNotFoundError, NotADirectoryError):
        return True
    except OSError:
        return False
    return False


class Root:
    def __init__(self, given: str) -> None:
        self.given = given
        self.files = 0
        self.bytes = 0
        self.links = 0
        self.special = 0
        self.filtered = 0
        self.unreadable: List[str] = []
        self.partial: List[str] = []
        self.found: List[Tuple[str, Dict[str, int]]] = []
        self.shapes: List[Tuple[str, int, int]] = []
        self.missing = False
        self.followed_link = False


def walk(root: str, name_filter: Optional[str], into: Root,
         label_of: Callable[[str], str] = lambda text: text) -> List[Tuple[str, str]]:
    """(path, path to print) of the regular files under `root`; links are not followed. `label_of`
    turns a relative path into the text to print (it hides a path that holds the key)."""
    out: List[Tuple[str, str]] = []
    try:
        info = os.lstat(root)
    except (FileNotFoundError, NotADirectoryError):
        into.missing = True
        return out
    except OSError:
        into.unreadable.append(".")  # it may exist: a folder on the way cannot be searched
        return out
    if stat.S_ISLNK(info.st_mode):
        try:
            info = os.stat(root)
        except (FileNotFoundError, NotADirectoryError):
            into.missing = True  # a link to nothing
            return out
        except OSError:
            into.unreadable.append(".")
            return out
        into.followed_link = True
    if stat.S_ISREG(info.st_mode):
        return [(root, os.path.basename(root) or root)]
    if not stat.S_ISDIR(info.st_mode):
        into.special += 1
        return out

    def failed(error: OSError) -> None:
        into.unreadable.append(label_of(os.path.relpath(error.filename, root)) if error.filename else "?")

    for folder, dirs, names in os.walk(root, onerror=failed, followlinks=False):
        for name in list(dirs):
            if os.path.islink(os.path.join(folder, name)):
                into.links += 1
        for name in sorted(names):
            path = os.path.join(folder, name)
            try:
                entry = os.lstat(path)
            except OSError:
                into.unreadable.append(label_of(os.path.relpath(path, root)))
                continue
            if stat.S_ISLNK(entry.st_mode):
                into.links += 1
                continue
            if not stat.S_ISREG(entry.st_mode):
                into.special += 1
                continue
            if name_filter and name_filter.lower() not in name.lower():
                into.filtered += 1
                continue
            out.append((path, os.path.relpath(path, root)))
    return out


def safe_path(text: str, needles: Sequence[bytes]) -> Tuple[str, bool]:
    """The path to print, or a placeholder when the path holds the key in one of its text forms."""
    encoded = text.encode("utf-8", errors="surrogateescape")
    for needle in needles:
        if needle in encoded:
            return "[a path that contains the key; not shown]", True
    return text, False


def home_as_tilde(text: str, home: str) -> str:
    """A path in the home folder as ~/...: the output is sent back, the account name stays here."""
    home = home.rstrip("/")
    if len(home) > 1 and (text == home or text.startswith(home + "/")):
        return "~" + text[len(home):]
    return text


def run(paths: Sequence[str], key: str, max_bytes: int, name_filter: Optional[str],
        chunk_bytes: int = CHUNK_BYTES, home: str = "") -> Tuple[List[str], int]:
    needles = encodings(key)
    piece = key_piece(key)
    key_bytes = [n for n in needles]
    hidden = key_bytes + ([piece] if piece is not None else [])
    lines = ["grep_key: looking for the key from %s (never shown) as raw text, JSON-escaped, URL-encoded, "
             "in base64 and in UTF-16, for its first %d characters, and for the shape sk-ant-... (another key or a "
             "test key). Not looked into: compressed files, base64 split over lines." % (VARIABLE, PIECE_CHARS), ""]
    roots: List[Root] = []
    for given in paths:
        root = Root(given)
        roots.append(root)
        expanded = os.path.expanduser(given)
        for path, shown in walk(expanded, name_filter, root, lambda text: safe_path(text, hidden)[0]):
            label, holds_key = safe_path(shown, hidden)
            counts: Dict[str, int] = {"in its path": 1} if holds_key else {}
            try:
                result = scan_file(path, needles, key_bytes, max_bytes, chunk_bytes, piece)
            except OSError:
                root.unreadable.append(label)
                if counts:
                    root.found.append((label, counts))
                continue
            root.files += 1
            root.bytes += result.scanned
            if result.partial:
                root.partial.append(label)
            counts.update(result.key)
            if counts:
                root.found.append((label, counts))
            if result.shapes:
                root.shapes.append((label, result.shapes, result.fake_shapes))
    found_files = 0
    scanned = 0
    missing = not_read = partial = 0
    for number, root in enumerate(roots, 1):
        shown_root, root_holds_key = safe_path(root.given, hidden)
        lines.append("Path %d: %s%s" % (number, home_as_tilde(shown_root, home),
                                        " (a link, followed)" if root.followed_link else ""))
        if root_holds_key:
            found_files += 1
        not_read += len(root.unreadable)
        partial += len(root.partial)
        if root.missing:
            missing += 1
            lines.append("  does not exist (skipped)")
            lines.append("")
            continue
        scanned += root.files
        notes = ["files scanned: %d (%.1f MiB)" % (root.files, root.bytes / (1024.0 * 1024.0))]
        if root.links:
            notes.append("links not followed: %d" % root.links)
        if root.special:
            notes.append("special files skipped: %d" % root.special)
        if root.filtered:
            notes.append("files without '%s' in the name skipped: %d" % (name_filter, root.filtered))
        lines.append("  " + "; ".join(notes))
        for label in root.unreadable:
            lines.append("  could not read: %s" % label)
        for label in root.partial:
            lines.append("  partly scanned (only the first %s): %s" % (size_label(max_bytes), label))
        for label, counts in root.found:
            found_files += 1
            lines.append("  KEY FOUND: %s: %s" % (label, ", ".join("%s %d" % (k, v) for k, v in counts.items())))
        real_shapes = [(label, n, fake) for label, n, fake in root.shapes if n > fake]
        fake_only = [(label, n) for label, n, fake in root.shapes if n == fake]
        for label, n, fake in real_shapes:
            lines.append("  shape sk-ant- (not the key): %s: %d%s" % (
                label, n, " (%d of them say FAKE)" % fake if fake else ""))
        if fake_only:
            lines.append("  shape sk-ant- with FAKE only (test keys): %d file(s), %d match(es)" % (
                len(fake_only), sum(n for _, n in fake_only)))
        lines.append("")
    if found_files:
        lines.append("Result: the key was FOUND in %d file(s) listed above. Do not open or send them; tell the "
                     "maintainer the file names." % found_files)
        return lines, 1
    skipped = " %d path(s) do not exist (skipped)." % missing if missing else ""
    if not_read or partial:
        lines.append("Result: NOT COMPLETE: the key was not found in what could be read (%d file(s) scanned), but %d "
                     "file(s) or folder(s) could not be read and %d file(s) were read only in part (listed above).%s"
                     % (scanned, not_read, partial, skipped))
        return lines, 3
    lines.append("Result: the key was found nowhere (%d file(s) scanned).%s" % (scanned, skipped))
    return lines, 0


class _QuietParser(argparse.ArgumentParser):
    """An argument parser whose errors are fixed texts: argparse's own messages repeat the bad
    argument, and an argument may hold a piece of the key."""

    def error(self, message: str) -> NoReturn:
        self.exit(2, "grep_key: usage error (see --help); the arguments are not repeated here\n")


def size_label(count: int) -> str:
    """A byte count in MiB, or in bytes below one MiB."""
    if count >= 1024 * 1024:
        return "%d MiB" % (count // (1024 * 1024))
    return "%d bytes" % count


def main(argv: Optional[Sequence[str]] = None, env: Optional[Dict[str, str]] = None) -> int:
    try:
        return run_main(argv, env)
    except Exception:
        # A fixed text only: an exception's message could hold a path or bytes of a file.
        print("grep_key: stopped by an internal error; the scan is not complete (tell the maintainer)",
              file=sys.stderr)
        return 2


def run_main(argv: Optional[Sequence[str]], env: Optional[Dict[str, str]]) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    environment = os.environ if env is None else env
    parser = _QuietParser(
        description="Look for the API key of %s in files; prints only paths and counts." % VARIABLE)
    parser.add_argument("paths", nargs="+", help="files or folders to scan (folders recursively)")
    parser.add_argument("--max-bytes", type=int, default=DEFAULT_MAX_BYTES,
                        help="read at most this many bytes of each file (default 256 MiB)")
    parser.add_argument("--name-contains", default=None,
                        help="inside folders, scan only files whose name contains this text (any case)")
    key = environment.get(VARIABLE, "")
    stripped = key.strip()
    # Before parsing: an argparse message could repeat an argument, and the key must not be in one.
    if stripped and any(stripped in arg for arg in argv):
        print("grep_key: the key was typed on the command line; nothing was scanned. Remove that line from "
              "the shell history.", file=sys.stderr)
        return 2
    try:
        args = parser.parse_args(argv)
    except SystemExit as stop:
        return 0 if stop.code == 0 else 2
    if not key:
        print("grep_key: %s is not set in this shell; export it first (hidden prompt, as in the sheet)." % VARIABLE,
              file=sys.stderr)
        return 2
    if len(stripped) < MIN_KEY_CHARS:
        print("grep_key: %s is shorter than %d characters; refused (it would match everywhere)." % (
            VARIABLE, MIN_KEY_CHARS), file=sys.stderr)
        return 2
    if args.max_bytes < 1:
        print("grep_key: --max-bytes must be at least 1", file=sys.stderr)
        return 2
    if all(absent(os.path.expanduser(p)) for p in args.paths):
        print("grep_key: none of the paths exists; nothing was scanned", file=sys.stderr)
        return 2
    lines, code = run(args.paths, stripped, args.max_bytes, args.name_contains,
                      home=environment.get("HOME", "") or os.path.expanduser("~"))
    print("\n".join(lines))
    return code


if __name__ == "__main__":
    sys.exit(main())
