#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A stand-in for Crusader Kings III's log writing, for tests that must run without the game.

It creates a CK3-style user folder (``logs/debug.log``, or the nested layout of a Wine/Proton
prefix) and appends to the log what the game would: synthetic engine-style noise lines and CURIA1
frames. Everything is synthetic: the engine prefix only mimics the SHAPE of a real one (119 to 131
bytes before the marker, observed in the M3 sessions) and is built at run time from invented
words, so no tracked file contains an engine log line. Frames come from the fixture format of
tests/fixtures/curia1/README.md or from the built-in example frame of docs/wire-format.md.

Use it as a command (see ``--help`` and README.md) or as a library (``FakeCk3``), as
tools/fake_ck3/e2e_test.py does. Standard library only; no fcntl, binary files, so it also runs on
Windows.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time
from typing import Callable, Iterable, List, Optional, Sequence

MARKER = b"CURIA1|"

_PREFIX_PARTS = (
    "synthetic-time",
    "synthetic-level",
    "synthetic-source:1",
    "synthetic/script/path.txt:1",
    "synthetic_effect_name:",
)

_NOISE_TEXTS = (
    "synthetic noise: nothing to report",
    "synthetic noise: an unrelated engine message",
    "synthetic noise: filler text with a pipe | in it",
)

_ERROR_TEXTS = (
    "synthetic error entry: something went wrong in a script",
    "    synthetic detail line one",
    "    synthetic detail line two",
)


def engine_prefix(variant: int = 0) -> bytes:
    """A synthetic engine-style prefix of 119 to 131 bytes (never contains the marker)."""
    target = 119 + variant % 13
    text = " ".join(_PREFIX_PARTS) + " "
    text += "." * (target - 1 - len(text)) + " "
    return text.encode("ascii")


def example_frame_records(snap: int) -> List[bytes]:
    """The example frame of docs/wire-format.md section 4 (artificial names), bare records."""
    p = "CURIA1|%d|" % snap
    rows = [
        "BEGIN|0|0.1.0|1066|9|15|1001|Test Ruler One",
        "FIN|312|78",
        "HEIR|1002|84|1|0|Test Heir Two",
        "VASSALS|2|2",
        "VASSAL|1|1003|120|55|1523|Test Vassal Three",
        "VASSAL|2|1004|40|-12|310|Test Vassal Four",
        "WARS|0",
        "CLAIMS|1|0|1",
        "CLAIM|1|weak|4|Test Realm",
        "FACTIONS|1|0|1",
        "FACTION|1|INFO|against|3|33|100|2",
        "FACTION|1|LEADER|1004|Test Vassal Four",
        "FACTION|1|CLAIMANT|1005|Test Claimant Five",
        "FACTION|1|TITLE|4|Test Realm",
        "FACTION|1|TARGET|1001|Test Ruler One",
        "END|16",
    ]
    return [(p + row).encode("utf-8") for row in rows]


class FixtureAction:
    """One step of a fixture: lines to append, or a truncation of the log."""

    def __init__(self, truncate: bool = False, lines: Optional[List[bytes]] = None):
        self.truncate = truncate
        self.lines = lines or []


def parse_fixture(data: bytes, prefix_variant_start: int = 0) -> List[FixtureAction]:
    """Reads the fixture format of tests/fixtures/curia1/README.md into actions.

    Lines come back complete with their ``\\n``-less bytes; bare records get the synthetic prefix.
    Raises ValueError on a line the format does not know.
    """
    actions: List[FixtureAction] = []
    current: List[bytes] = []
    variant = prefix_variant_start
    rows = data.split(b"\n")
    if rows and rows[-1] == b"":
        rows.pop()  # the text after the final newline
    for number, raw in enumerate(rows):
        line = raw[:-1] if raw.endswith(b"\r") else raw
        if line == b"" or line.startswith(b"#"):
            continue
        if line == b"!truncate":
            if current:
                actions.append(FixtureAction(lines=current))
                current = []
            actions.append(FixtureAction(truncate=True))
        elif line in (b"!noise", b"!raw"):
            current.append(b"")
        elif line.startswith(b"!noise "):
            current.append(line[len(b"!noise "):])
        elif line.startswith(b"!raw "):
            current.append(line[len(b"!raw "):])
        elif line.startswith(MARKER):
            current.append(engine_prefix(variant) + line)
        else:
            raise ValueError("unknown fixture line %d: %r" % (number + 1, line[:40]))
        variant += 1
    if current:
        actions.append(FixtureAction(lines=current))
    return actions


class FakeCk3:
    """The fake game: owns a user folder and appends to its ``debug.log``.

    ``line_buffer``     write every line in two chunks (a reader can catch a torn line)
    ``flush_delay``     seconds the data sits in the "process" before it reaches the file
    ``burst``           write all lines of a frame (or fixture step) in one write instead of one
                        write per line
    ``interleave``      insert noise lines and multi-line error entries inside frames
    ``die_after_bytes`` stop writing for good once this many bytes were written (the cut may fall
                        inside a line), simulating the cap on the game's log
    ``fsync``           ``os.fsync`` after every write
    """

    def __init__(
        self,
        root: Optional[str] = None,
        layout: str = "plain",
        line_buffer: bool = False,
        flush_delay: float = 0.0,
        burst: bool = False,
        interleave: bool = False,
        die_after_bytes: Optional[int] = None,
        fsync: bool = False,
        snap_start: int = 1,
        chunk_pause: float = 0.002,
        sleep: Callable[[float], None] = time.sleep,
        keep_chunks: bool = False,
        keep: bool = False,
    ):
        if layout not in ("plain", "wine"):
            raise ValueError("layout must be 'plain' or 'wine'")
        self._owns_root = root is None and not keep
        self.root = root if root is not None else tempfile.mkdtemp(prefix="fake_ck3_")
        self.layout = layout
        self.line_buffer = line_buffer
        self.flush_delay = flush_delay
        self.burst = burst
        self.interleave = interleave
        self.die_after_bytes = die_after_bytes
        self.fsync = fsync
        self.next_snap = snap_start
        self.chunk_pause = chunk_pause
        self._sleep = sleep
        self.written = 0
        self.dead = False
        # Sizes of the individual writes, when asked for (tests of the write pattern).
        self.chunks: Optional[List[int]] = [] if keep_chunks else None
        self._variant = 0
        self._pending_tail: List[bytes] = []
        self._fh = None
        if layout == "wine":
            parts = ["pfx", "drive_c", "users", "steamuser", "Documents", "Paradox Interactive",
                     "Crusader Kings III", "logs"]
            self.logs_dir = os.path.join(self.root, *parts)
        else:
            self.logs_dir = os.path.join(self.root, "logs")
        self.log_path = os.path.join(self.logs_dir, "debug.log")

    # -- lifecycle -------------------------------------------------------------------------

    def launch(self, truncate: bool = False) -> None:
        """Creates the folder and the log if needed; with ``truncate`` the log is emptied in place
        (same file, size back to 0), as the game does at launch (M3 T5)."""
        os.makedirs(self.logs_dir, exist_ok=True)
        if not os.path.exists(self.log_path):
            with open(self.log_path, "wb"):
                pass
        elif truncate:
            self.truncate()

    def truncate(self) -> None:
        with open(self.log_path, "r+b") as handle:
            handle.truncate(0)
        self._pending_tail = []

    def size(self) -> int:
        return os.path.getsize(self.log_path) if os.path.exists(self.log_path) else 0

    def close(self) -> None:
        if self._fh is not None:
            self._fh.close()
            self._fh = None
        if self._owns_root:
            shutil.rmtree(self.root, ignore_errors=True)

    def __enter__(self) -> "FakeCk3":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # -- the lowest layer: bytes into the file ----------------------------------------------

    def _write_chunk(self, data: bytes) -> None:
        if self._fh is None:
            self.launch()
            # Unbuffered and appending, like a game that flushes every line.
            self._fh = open(self.log_path, "ab", buffering=0)
        self._fh.write(data)
        if self.fsync:
            os.fsync(self._fh.fileno())
        if self.chunks is not None:
            self.chunks.append(len(data))

    def _emit(self, data: bytes) -> None:
        """One logical write of the game: honours the cap, the flush delay and line buffering."""
        if self.dead or not data:
            return
        if self.die_after_bytes is not None:
            room = self.die_after_bytes - self.written
            if len(data) >= room:
                data = data[: max(room, 0)]
                self.dead = True
        if not data:
            return
        if self.flush_delay > 0:
            self._sleep(self.flush_delay)
        if self.line_buffer and len(data) > 1:
            middle = len(data) // 2
            self._write_chunk(data[:middle])
            self._sleep(self.chunk_pause)
            self._write_chunk(data[middle:])
        else:
            self._write_chunk(data)
        self.written += len(data)

    def write_lines(self, lines: Sequence[bytes], burst: Optional[bool] = None) -> None:
        """Appends complete lines (each gets its ``\\n``): one write per line, or all in one."""
        use_burst = self.burst if burst is None else burst
        if use_burst:
            self._emit(b"".join(line + b"\n" for line in lines))
        else:
            for line in lines:
                self._emit(line + b"\n")

    # -- what the game writes ---------------------------------------------------------------

    def _prefixed(self, record: bytes) -> bytes:
        line = engine_prefix(self._variant) + record
        self._variant += 1
        return line

    def noise_lines(self, count: int = 1) -> List[bytes]:
        lines = []
        for i in range(count):
            lines.append(self._prefixed(_NOISE_TEXTS[(self._variant + i) % len(_NOISE_TEXTS)].encode()))
        return lines

    def error_entry_lines(self) -> List[bytes]:
        """A multi-line error entry: a prefixed first line and continuation lines without prefix."""
        first, *rest = (t.encode() for t in _ERROR_TEXTS)
        return [self._prefixed(first)] + rest

    def append_noise(self, count: int = 1) -> None:
        self.write_lines(self.noise_lines(count))

    def append_error_entry(self) -> None:
        self.write_lines(self.error_entry_lines())

    def _interleaved(self, records: Sequence[bytes]) -> List[bytes]:
        """Prefixed record lines with noise and error entries between them (never before the
        first or after the last, so they sit inside the frame)."""
        lines: List[bytes] = []
        for index, record in enumerate(records):
            if index > 0 and index % 3 == 0:
                lines.extend(self.noise_lines(1))
            if index > 0 and index % 5 == 0:
                lines.extend(self.error_entry_lines())
            lines.append(self._prefixed(record))
        return lines

    def append_records(self, records: Sequence[bytes], interleave: Optional[bool] = None,
                       burst: Optional[bool] = None) -> None:
        """Appends bare ``CURIA1|...`` records, each with the synthetic prefix."""
        use_interleave = self.interleave if interleave is None else interleave
        if use_interleave:
            lines = self._interleaved(records)
        else:
            lines = [self._prefixed(record) for record in records]
        self.write_lines(lines, burst)

    def append_frame(self, records: Optional[Sequence[bytes]] = None, snap: Optional[int] = None,
                     partial: bool = False) -> int:
        """Appends one frame (the example frame unless ``records`` is given) and returns its snap.

        ``partial`` stops before the END record; ``finish_frame()`` writes the rest.
        """
        if snap is None:
            snap = self.next_snap
        self.next_snap = snap + 1
        frame = list(records) if records is not None else example_frame_records(snap)
        if partial:
            self._pending_tail = frame[-1:]
            frame = frame[:-1]
        else:
            self._pending_tail = []
        self.append_records(frame)
        return snap

    def finish_frame(self) -> None:
        """Writes the records held back by ``append_frame(partial=True)``."""
        tail, self._pending_tail = self._pending_tail, []
        if tail:
            self.append_records(tail, interleave=False)

    def play_fixture(self, path: str, between: Optional[Callable[[str], None]] = None) -> None:
        """Plays a fixture file (tests/fixtures/curia1/README.md).

        ``between('truncate')`` is called just before each truncation, so that a harness can wait
        until the reader has caught up (otherwise bytes written just before the truncation could be
        lost, as with the real game), and ``between('truncated')`` just after it, so that it can wait
        until the reader noticed before more bytes arrive."""
        with open(path, "rb") as handle:
            actions = parse_fixture(handle.read(), self._variant)
        self._variant += sum(len(action.lines) for action in actions)
        for action in actions:
            if action.truncate:
                if between is not None:
                    between("truncate")
                self.truncate()
                if between is not None:
                    between("truncated")
            else:
                self.write_lines(action.lines)


# -- command line -----------------------------------------------------------------------------


def run_commands(fake: FakeCk3, lines: Iterable[str], out=None) -> None:
    """The tiny stdin command language: frame, partial, rest, noise [n], error, truncate,
    fixture <path>, size, quit."""
    out = out if out is not None else sys.stdout
    for raw in lines:
        words = raw.split()
        if not words:
            continue
        command = words[0]
        if command == "quit":
            return
        elif command == "frame":
            print("snap %d" % fake.append_frame(), file=out, flush=True)
        elif command == "partial":
            print("snap %d" % fake.append_frame(partial=True), file=out, flush=True)
        elif command == "rest":
            fake.finish_frame()
        elif command == "noise":
            fake.append_noise(int(words[1]) if len(words) > 1 else 1)
        elif command == "error":
            fake.append_error_entry()
        elif command == "truncate":
            fake.truncate()
        elif command == "fixture" and len(words) > 1:
            fake.play_fixture(" ".join(words[1:]))
        elif command == "size":
            print("size %d" % fake.size(), file=out, flush=True)
        else:
            print("unknown command: %s" % raw.strip(), file=sys.stderr, flush=True)
            continue
        print("ok", file=out, flush=True)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Fake Crusader Kings III log writer (synthetic noise and CURIA1 frames).")
    p.add_argument("--root", help="user folder to use (default: a temporary one, removed on exit)")
    p.add_argument("--layout", choices=("plain", "wine"), default="plain",
                   help="plain: <root>/logs/debug.log; wine: the nested layout of a Wine/Proton prefix")
    p.add_argument("--fixture", action="append", default=[],
                   help="play this fixture file instead of the built-in example frame (repeatable)")
    p.add_argument("--repeat", type=int, default=1, help="how many frames (or fixture rounds) to write")
    p.add_argument("--interval", type=float, default=0.0, help="seconds between repeats")
    p.add_argument("--snap-start", type=int, default=1, help="snap of the first generated frame")
    p.add_argument("--noise", type=int, default=0, help="noise lines to write before the frames")
    p.add_argument("--prior-frames", type=int, default=0,
                   help="frames of a 'previous session' already in the log before the launch")
    p.add_argument("--launch-delay", type=float, default=0.0,
                   help="seconds between the previous session and the launch (a reader can attach)")
    p.add_argument("--truncate-at-launch", action="store_true",
                   help="empty the existing log in place at launch (same file, size 0)")
    p.add_argument("--line-buffer", action="store_true", help="write each line in two chunks")
    p.add_argument("--flush-delay", type=float, default=0.0,
                   help="seconds each write is held back before it reaches the file")
    p.add_argument("--burst", action="store_true", help="all lines of a frame in one write")
    p.add_argument("--partial-frame", action="store_true",
                   help="after the frames, write one more that stops before END")
    p.add_argument("--interleave", action="store_true",
                   help="insert noise lines and multi-line error entries inside frames")
    p.add_argument("--die-after-bytes", type=int, default=None,
                   help="stop writing for good after this many bytes (simulates the log cap)")
    p.add_argument("--fsync", action="store_true", help="os.fsync after every write")
    p.add_argument("--stdin", action="store_true",
                   help="after the workload read commands from stdin (frame, partial, rest, noise [n], "
                        "error, truncate, fixture <path>, size, quit)")
    p.add_argument("--keep", action="store_true", help="do not remove a temporary root on exit")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    fake = FakeCk3(
        root=args.root,
        layout=args.layout,
        line_buffer=args.line_buffer,
        flush_delay=args.flush_delay,
        burst=args.burst,
        interleave=args.interleave,
        die_after_bytes=args.die_after_bytes,
        fsync=args.fsync,
        snap_start=args.snap_start,
        keep=args.keep,
    )
    try:
        # Previous session: written with plain per-line writes and no scenario flags.
        fake.launch()
        print("LOG %s" % fake.log_path, flush=True)
        if args.prior_frames:
            saved = (fake.line_buffer, fake.flush_delay, fake.interleave, fake.die_after_bytes)
            fake.line_buffer, fake.flush_delay, fake.interleave, fake.die_after_bytes = False, 0.0, False, None
            for _ in range(args.prior_frames):
                fake.append_frame()
            fake.line_buffer, fake.flush_delay, fake.interleave, fake.die_after_bytes = saved
            fake.written = 0
            fake.next_snap = args.snap_start
        if args.launch_delay > 0:
            time.sleep(args.launch_delay)
        if args.truncate_at_launch:
            fake.truncate()
        if args.noise:
            fake.append_noise(args.noise)
        for round_number in range(args.repeat):
            if round_number > 0 and args.interval > 0:
                time.sleep(args.interval)
            if args.fixture:
                for path in args.fixture:
                    fake.play_fixture(path)
            else:
                fake.append_frame()
        if args.partial_frame:
            fake.append_frame(partial=True)
        print("DONE %d" % fake.size(), flush=True)
        if args.stdin:
            run_commands(fake, sys.stdin)
    except KeyboardInterrupt:
        return 130
    finally:
        fake.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
