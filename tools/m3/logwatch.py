#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Milestone 3 log watcher: measures CK3's debug.log behaviour while the game runs (read-only).

It polls debug.log and error.log with os.stat, reads the appended bytes and records, for every
line carrying the CURIA1| marker: the arrival time, the engine's own timestamp, the two latency
bounds this pair permits, the line size and the text after the marker. It also tracks frames
(BEGIN .. END groups by snap_id), log truncation or re-creation (T5), the bytes appended to each
log per minute with and without the CURIA lines (T8), and error.log lines mentioning the mod.

Python 3.9+, standard library only. Never writes to the game folders. Run it before launching CK3
(it waits for debug.log) and type a label word plus Enter in the terminal to tag what the game is
doing (paused, menu, fast, normal ...); a line starting with '!' is stored as a free-text note.

  python3 tools/m3/logwatch.py                  # watch until Ctrl+C
  python3 tools/m3/logwatch.py --duration 600   # watch for ten minutes
  python3 tools/m3/logwatch.py --selftest       # run the built-in scenarios, no CK3 needed

Outputs go to ~/curia_m3_results/ (--out-dir): watch-<stamp>.jsonl (one event per line),
raw-curia-<stamp>.txt (the CURIA1 lines as seen, engine prefix and the line terminator included) and
summary-<stamp>.txt. Those files hold engine text and stay local; see tools/m3/README.md.
"""
from __future__ import annotations

import argparse
import collections
import dataclasses
import datetime
import hashlib
import json
import os
import platform
import queue
import re
import shutil
import signal
import statistics
import sys
import tempfile
import threading
import time
import traceback
from typing import Any, Callable, Counter, Dict, IO, List, Optional, Set, Tuple

TOOL_VERSION = "1"
MARKER = b"CURIA1|"
BOM = b"\xef\xbb\xbf"
DEFAULT_MOD_PATTERN = r"curia[_/]"
# Engine line prefix: [HH:MM:SS][level][source:line]
PREFIX_RE = re.compile(rb"^\[(\d\d):(\d\d):(\d\d)\]\[([A-Za-z]+)\]\[([^\]:]+):(\d+)\]")
DAY_SECONDS = 86400.0
FRAME_MARKER_KINDS = (b"BEGIN", b"END")


# --------------------------------------------------------------------------------------------
# Pure helpers: prefix parsing, clock arithmetic, formatting
# --------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class EnginePrefix:
    """The engine's bracketed prefix of a log line."""

    second_of_day: int  # whole seconds since local midnight (the engine clock has no sub-second part)
    level: str
    source: str  # "file:line" without the brackets

    @property
    def clock(self) -> str:
        s = self.second_of_day
        return f"{s // 3600:02d}:{(s // 60) % 60:02d}:{s % 60:02d}"


def parse_prefix(line: bytes) -> Optional[EnginePrefix]:
    """Parse the engine prefix at the start of a line; None when the line has no such prefix."""
    m = PREFIX_RE.match(line)
    if m is None:
        return None
    hh, mm, ss = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if hh > 23 or mm > 59 or ss > 59:
        return None
    source = m.group(5).decode("ascii", "replace") + ":" + m.group(6).decode("ascii")
    return EnginePrefix(hh * 3600 + mm * 60 + ss, m.group(4).decode("ascii"), source)


def find_marker(line: bytes) -> int:
    """Offset of the CURIA1| marker anywhere in the line, or -1."""
    return line.find(MARKER)


def seconds_of_day(wall: float) -> float:
    """Local seconds since midnight for a wall-clock reading (sub-second part kept)."""
    dt = datetime.datetime.fromtimestamp(wall)
    return dt.hour * 3600 + dt.minute * 60 + dt.second + dt.microsecond / 1e6


def raw_difference(arrival_sod: float, engine_sod: float) -> float:
    """Arrival minus engine second in seconds, folded into [-12 h, +12 h).

    The fold makes midnight rollover (engine 23:59:59, arrival 00:00:00.5) read as 1.5 s instead
    of -86398.5 s. A genuine offset between the two clocks within +-12 h is NOT folded away; it
    shows up in the bounds and in the clock-offset section of the summary.
    """
    d = arrival_sod - engine_sod
    return ((d + DAY_SECONDS / 2) % DAY_SECONDS) - DAY_SECONDS / 2


def latency_bounds(arrival_sod: float, engine_sod: float) -> Tuple[float, float]:
    """(lower, upper) bounds on the delay between the engine writing a line and the watcher seeing it.

    The engine stamps whole seconds: the true time t satisfies engine <= t < engine + 1, so with the
    arrival A the latency A - t lies in (A - (engine + 1), A - engine].
    """
    d = raw_difference(arrival_sod, engine_sod)
    return d - 1.0, d


def fmt_wall(wall: float) -> str:
    """Local time with millisecond resolution."""
    dt = datetime.datetime.fromtimestamp(wall)
    return dt.strftime("%Y-%m-%d %H:%M:%S.") + f"{dt.microsecond // 1000:03d}"


def fmt_clock(wall: float) -> str:
    return fmt_wall(wall)[11:]


def fmt_bytes(n: float) -> str:
    n = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024.0 or unit == "GB":
            return f"{n:,.0f} {unit}" if unit == "B" else f"{n:,.1f} {unit}"
        n /= 1024.0
    return f"{n:,.1f} GB"


def fmt_s(x: Optional[float]) -> str:
    return "n/a" if x is None else f"{x:+.3f}"


def mmm(values: List[float]) -> Tuple[Optional[float], Optional[float], Optional[float]]:
    """(min, median, max) of a list, or Nones when empty."""
    if not values:
        return None, None, None
    return min(values), statistics.median(values), max(values)


def decode_replace(data: bytes) -> str:
    return data.decode("utf-8", "replace")


def utf8_error_offset(data: bytes) -> Optional[int]:
    """Byte offset of the first invalid UTF-8 sequence, or None when the data is valid."""
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as exc:
        return exc.start
    return None


def odd_bytes(payload: bytes, limit: int = 10) -> List[Tuple[int, str]]:
    """Offsets (within the payload) of non-ASCII bytes and control characters, with a readable form."""
    found: List[Tuple[int, str]] = []
    for i, b in enumerate(payload):
        if b >= 0x80 or (b < 0x20 and b != 0x09) or b == 0x7F:
            found.append((i, f"0x{b:02x}"))
            if len(found) >= limit:
                break
    return found


# --------------------------------------------------------------------------------------------
# Clock (injectable so tests control time)
# --------------------------------------------------------------------------------------------


class Clock:
    """Wall clock, monotonic clock and sleep. Tests substitute a fake."""

    def wall(self) -> float:
        return time.time()

    def mono(self) -> float:
        return time.monotonic()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)


@dataclasses.dataclass(frozen=True)
class Arrival:
    wall: float
    mono: float


# --------------------------------------------------------------------------------------------
# Line assembly
# --------------------------------------------------------------------------------------------


@dataclasses.dataclass
class Line:
    data: bytes  # without the line terminator
    raw_len: int  # bytes the line occupies in the file, terminator included
    crlf: bool  # terminated by CR LF
    reads: int  # how many reads it took to receive the complete line (1 = whole in one read)
    offset: int  # file offset of the first byte
    capped: bool = False  # emitted early because the partial-line buffer hit its cap


class LineAssembler:
    """Turns appended byte chunks into complete lines, keeping the unterminated tail between reads."""

    def __init__(self, start_offset: int = 0, max_partial: int = 8 * 1024 * 1024, drop_head: bool = False):
        self.partial = b""
        self.partial_reads = 0
        self.offset = start_offset  # file offset of the start of the pending (partial or next) line
        self.max_partial = max_partial
        self.drop_head = drop_head  # attached mid-line: skip bytes up to the first newline
        self.dropped_head_bytes = 0

    def feed(self, data: bytes) -> List[Line]:
        lines: List[Line] = []
        if self.drop_head:
            nl = data.find(b"\n")
            if nl < 0:
                self.dropped_head_bytes += len(data)
                self.offset += len(data)
                return lines
            self.dropped_head_bytes += nl + 1
            self.offset += nl + 1
            data = data[nl + 1 :]
            self.drop_head = False
        buf = self.partial + data
        start = 0
        first = True
        while True:
            nl = buf.find(b"\n", start)
            if nl < 0:
                break
            raw = buf[start : nl + 1]
            body = raw[:-1]
            crlf = body.endswith(b"\r")
            if crlf:
                body = body[:-1]
            reads = self.partial_reads + 1 if first else 1
            lines.append(Line(body, len(raw), crlf, reads, self.offset))
            self.offset += len(raw)
            start = nl + 1
            first = False
        rest = buf[start:]
        if not rest:
            self.partial_reads = 0
        elif first:
            self.partial_reads += 1  # still the same line, one more read contributed
        else:
            self.partial_reads = 1  # a new line began in this read
        if len(rest) > self.max_partial:
            lines.append(Line(rest, len(rest), False, self.partial_reads, self.offset, capped=True))
            self.offset += len(rest)
            rest = b""
            self.partial_reads = 0
        self.partial = rest
        return lines

    def reset(self, start_offset: int = 0) -> int:
        """Discard the pending partial line (file truncated); returns the discarded byte count."""
        discarded = len(self.partial)
        self.partial = b""
        self.partial_reads = 0
        self.offset = start_offset
        self.drop_head = False
        return discarded


# --------------------------------------------------------------------------------------------
# File tailing (os.stat polling)
# --------------------------------------------------------------------------------------------


@dataclasses.dataclass
class TailLines:
    name: str
    lines: List[Line]
    arrival: Arrival
    backlog: bool  # content that existed before the watcher attached (--from-start)
    mtime: float
    size: int
    data_len: int


@dataclasses.dataclass
class TailFileEvent:
    """attached, appeared, truncated, recreated or disappeared."""

    name: str
    kind: str
    arrival: Arrival
    old_size: int
    new_size: int
    discarded_partial: int = 0
    bom: Optional[bool] = None


@dataclasses.dataclass
class TailBom:
    """The BOM answer for a file whose first bytes were read in a later poll than its start event."""

    name: str
    bom: bool
    arrival: Arrival


TailEvent = Any  # TailLines, TailFileEvent or TailBom


class FileTailer:
    """Follows one log file by polling os.stat and reading the appended bytes.

    Handles: the file not existing yet, truncation (same inode, smaller size), re-creation (new
    inode), disappearance, a growing file, and an unterminated last line. A truncation followed by
    regrowth beyond the old position between two polls on the same inode cannot be seen.
    """

    def __init__(self, path: str, name: str, clock: Clock, from_start: bool, max_partial: int):
        self.path = path
        self.name = name
        self.clock = clock
        self.from_start = from_start
        self.max_partial = max_partial
        self.pos = 0
        self.key: Optional[Tuple[int, int]] = None
        self.size = 0
        self.first_poll = True
        self.waiting_reported = False
        self.bom_pending = False  # the file start (first 3 bytes) has not been read yet
        self.bom_head = b""
        self.assembler = LineAssembler(max_partial=max_partial)

    def _arrival(self) -> Arrival:
        return Arrival(self.clock.wall(), self.clock.mono())

    def poll(self) -> List[TailEvent]:
        events: List[TailEvent] = []
        try:
            st = os.stat(self.path)
        except OSError:
            arrival = self._arrival()
            self.first_poll = False
            if self.key is not None:
                events.append(
                    TailFileEvent(self.name, "disappeared", arrival, self.pos, 0, self.assembler.reset(0))
                )
                self.key = None
                self.pos = 0
                self.size = 0
                self.bom_pending = False
                self.bom_head = b""
                self.waiting_reported = True  # reported by the disappeared event
            elif not self.waiting_reported:
                self.waiting_reported = True
                events.append(TailFileEvent(self.name, "waiting", arrival, 0, 0))
            return events
        arrival = self._arrival()  # taken right after stat: the bytes were readable at this moment
        key = (st.st_dev, st.st_ino)
        backlog = False
        if self.key is None:
            if self.first_poll:
                events.extend(self._attach(st, arrival))
                backlog = self.from_start
            else:
                events.append(TailFileEvent(self.name, "appeared", arrival, 0, st.st_size))
                self.pos = 0
                self.assembler.reset(0)
                self._expect_bom()
            self.key = key
            self.first_poll = False
        elif key != self.key:
            events.append(
                TailFileEvent(self.name, "recreated", arrival, self.pos, st.st_size, self.assembler.reset(0))
            )
            self.key = key
            self.pos = 0
            self._expect_bom()
        elif st.st_size < self.pos:
            events.append(
                TailFileEvent(self.name, "truncated", arrival, self.pos, st.st_size, self.assembler.reset(0))
            )
            self.pos = 0
            self._expect_bom()
        self.size = st.st_size
        if st.st_size > self.pos:
            result = self._read(arrival, backlog, events)
            if result is not None:
                events.append(result)
        return events

    def _attach(self, st: os.stat_result, arrival: Arrival) -> List[TailEvent]:
        """First sight of an existing file: start at the end (default) or read it all as backlog."""
        events: List[TailEvent] = []
        if self.from_start:
            self.pos = 0
            self.assembler.reset(0)
        else:
            self.pos = st.st_size
            self.assembler.reset(st.st_size)
            if st.st_size > 0 and not self._previous_byte_is_newline(st.st_size):
                self.assembler.drop_head = True
        bom = self._has_bom() if self.pos == 0 else None
        if self.pos == 0 and bom is None:
            self._expect_bom()  # empty (or too short to tell): answered by the read that gets the first bytes
        events.append(TailFileEvent(self.name, "attached", arrival, 0, st.st_size, bom=bom))
        return events

    def _expect_bom(self) -> None:
        self.bom_pending = True
        self.bom_head = b""

    def _previous_byte_is_newline(self, pos: int) -> bool:
        try:
            with open(self.path, "rb") as f:
                f.seek(pos - 1)
                return f.read(1) == b"\n"
        except OSError:
            return True

    def _has_bom(self) -> Optional[bool]:
        try:
            with open(self.path, "rb") as f:
                head = f.read(3)
        except OSError:
            return None
        if len(head) < len(BOM) and (not head or BOM.startswith(head)):
            return None  # not enough bytes yet to tell
        return head == BOM

    def _read(self, arrival: Arrival, backlog: bool, events: List[TailEvent]) -> Optional[TailLines]:
        chunks: List[bytes] = []
        try:
            with open(self.path, "rb") as f:
                fst = os.fstat(f.fileno())
                if (fst.st_dev, fst.st_ino) != self.key:
                    return None  # replaced between stat and open: the next poll reports it
                f.seek(self.pos)
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    chunks.append(chunk)
        except OSError:
            return None
        data = b"".join(chunks)
        if not data:
            return None
        if self.bom_pending:
            # The BOM question (T4): answered from the first bytes of a file after attach, appearance,
            # re-creation or truncation, whether or not they arrive in the poll that reported the event.
            head = (self.bom_head + data)[: len(BOM)]
            if len(head) >= len(BOM) or not BOM.startswith(head):
                self.bom_pending = False
                self.bom_head = b""
                bom = head == BOM
                same_poll = [ev for ev in events if isinstance(ev, TailFileEvent) and ev.name == self.name and ev.bom is None]
                for ev in same_poll:
                    ev.bom = bom
                if not same_poll:
                    events.append(TailBom(self.name, bom, arrival))
            else:
                self.bom_head = head
        self.pos += len(data)
        lines = self.assembler.feed(data)
        return TailLines(self.name, lines, arrival, backlog, fst.st_mtime, fst.st_size, len(data))


# --------------------------------------------------------------------------------------------
# Labels and notes (stdin)
# --------------------------------------------------------------------------------------------


class LabelState:
    """Current label plus a queue of label/note events, shared with the stdin reader thread."""

    def __init__(self, clock: Clock, initial: str = "none"):
        self._clock = clock
        self._label = initial
        self._lock = threading.Lock()
        self._events: "queue.Queue[Dict[str, Any]]" = queue.Queue()

    def current(self) -> str:
        with self._lock:
            return self._label

    def apply_line(self, text: str) -> None:
        """One typed line: '!text' is a note, any other non-empty text becomes the label."""
        text = text.strip()
        if not text:
            return
        wall, mono = self._clock.wall(), self._clock.mono()
        if text.startswith("!"):
            self._events.put({"type": "note", "text": text[1:].strip(), "wall": wall, "mono": mono})
            return
        with self._lock:
            old, self._label = self._label, text
        self._events.put({"type": "label", "label": text, "previous": old, "wall": wall, "mono": mono})

    def drain(self) -> List[Dict[str, Any]]:
        out: List[Dict[str, Any]] = []
        while True:
            try:
                out.append(self._events.get_nowait())
            except queue.Empty:
                return out


def start_label_reader(stream: IO[str], state: LabelState) -> threading.Thread:
    """Daemon thread feeding lines from a text stream (stdin, or a pipe in tests) to the label state."""

    def loop() -> None:
        try:
            for line in stream:
                state.apply_line(line)
        except (OSError, ValueError):
            pass

    t = threading.Thread(target=loop, name="label-reader", daemon=True)
    t.start()
    return t


# --------------------------------------------------------------------------------------------
# Analysis (no I/O): samples, frames, budget, anomalies
# --------------------------------------------------------------------------------------------


@dataclasses.dataclass
class Config:
    log_dir: str
    out_dir: str
    interval_ms: float = 10.0
    duration: float = 0.0  # seconds; 0 = until interrupted
    report_every: float = 30.0  # seconds; 0 = off
    from_start: bool = False
    count_convention: str = "all"  # END count includes BEGIN and END ("all") or the body lines only ("body")
    long_line_bytes: int = 4000
    engine_offset_s: float = 0.0  # added to the engine clock before computing bounds (negative if the engine clock is ahead)
    mod_pattern: str = DEFAULT_MOD_PATTERN  # case-insensitive regex: which error.log lines count as mentioning the mod
    max_partial_bytes: int = 8 * 1024 * 1024
    use_stdin: bool = True
    quiet: bool = False
    initial_label: str = "none"
    files: Tuple[str, ...] = ("debug.log", "error.log")


@dataclasses.dataclass
class Sample:
    label: str
    arrival_wall: float
    engine_sod: Optional[float]  # engine second of day with --engine-offset-s applied
    lower: Optional[float]
    upper: Optional[float]
    line_bytes: int
    raw_bytes: int
    reads: int
    backlog: bool


@dataclasses.dataclass
class Frame:
    snap_id: str
    label: str
    first_wall: float
    last_wall: float
    first_seq: int
    last_seq: int
    noncuria_first: int
    noncuria_last: int
    lines: int = 0
    body_lines: int = 0
    payload_bytes: int = 0
    raw_bytes: int = 0
    longest: int = 0
    has_begin: bool = False
    has_end: bool = False
    declared: Optional[int] = None
    backlog: bool = False
    kinds: List[str] = dataclasses.field(default_factory=list)
    session: int = 1


class Analysis:
    """Collects everything the summary reports. Receives lines; performs no file or console I/O."""

    def __init__(self, cfg: Config, emit: Callable[[Dict[str, Any]], None], emit_raw: Callable[[bytes], None]):
        self.cfg = cfg
        self.emit = emit
        self.emit_raw = emit_raw
        self.mod_re = re.compile(cfg.mod_pattern.encode("utf-8"), re.IGNORECASE)
        self.session = 1  # incremented when debug.log is truncated, re-created or removed (a new game launch)
        self.samples: List[Sample] = []
        self.frames_done: List[Dict[str, Any]] = []
        self.open_frames: Dict[str, Frame] = {}
        self.done_ids: Set[str] = set()
        self.seen_payloads: Set[bytes] = set()
        self.anomalies: List[Dict[str, Any]] = []
        self.anomaly_counts: Counter[str] = collections.Counter()
        self.debug_seq = 0
        self.noncuria_count = 0
        self.curia_count = 0
        self.curia_no_prefix = 0
        self.curia_lengths: List[int] = []
        self.curia_prefix_lengths: List[int] = []
        self.prefix_sources: Counter[str] = collections.Counter()
        self.diff_floor_counts: Counter[int] = collections.Counter()  # floor(arrival - engine second), all prefixed lines
        self.crlf_lines: Dict[str, int] = collections.defaultdict(int)
        self.invalid_utf8_other: Dict[str, int] = collections.defaultdict(int)
        self.long_other_lines = 0
        self.error_hits: List[Dict[str, Any]] = []
        self.file_events: List[Dict[str, Any]] = []
        self.sizes: Dict[str, Dict[str, Any]] = {}
        # byte budget: file -> kind ('other' | 'curia') -> total, and per-minute buckets
        self.budget_total: Dict[str, Dict[str, int]] = collections.defaultdict(lambda: {"other": 0, "curia": 0})
        self.budget_minutes: Dict[str, Dict[int, Dict[str, int]]] = collections.defaultdict(dict)
        self.last_growth: Dict[str, float] = {}
        self.reads_per_line_gt1 = 0
        self.start_mono: Optional[float] = None  # first appended byte seen: origin of the minute buckets and the rates
        self.last_mono: Optional[float] = None  # most recent appended byte seen

    # ---- bookkeeping -------------------------------------------------------------------------

    def anomaly(self, kind: str, detail: str, wall: float, **extra: Any) -> None:
        rec = {"type": "anomaly", "kind": kind, "detail": detail, "wall": wall, "time": fmt_wall(wall)}
        rec.update(extra)
        self.anomalies.append(rec)
        self.anomaly_counts[kind] += 1
        self.emit(rec)

    def note_file_event(self, ev: TailFileEvent) -> None:
        rec = {
            "type": "truncation" if ev.kind in ("truncated", "recreated", "disappeared") else "file",
            "file": ev.name,
            "event": ev.kind,
            "old_size": ev.old_size,
            "new_size": ev.new_size,
            "discarded_partial_bytes": ev.discarded_partial,
            "bom": ev.bom,
            "wall": ev.arrival.wall,
            "time": fmt_wall(ev.arrival.wall),
            "mono": round(ev.arrival.mono, 3),
        }
        self.file_events.append(rec)
        self.emit(rec)
        if ev.kind in ("attached", "appeared"):
            self.sizes[ev.name] = {"first_size": ev.new_size, "bom": ev.bom}
        if ev.name == "debug.log" and ev.kind in ("truncated", "recreated", "disappeared"):
            self.reset_session(ev.kind, ev.arrival.wall)

    def note_bom(self, bom: TailBom) -> None:
        """Attach a late BOM answer to the latest start event of that file that has none."""
        target = None
        for rec in reversed(self.file_events):
            if rec["file"] == bom.name and rec["event"] in ("attached", "appeared", "recreated", "truncated"):
                if rec["bom"] is None:
                    target = rec
                break
        if target is not None:
            target["bom"] = bom.bom
            info = self.sizes.get(bom.name)
            if info is not None and target["event"] in ("attached", "appeared") and info.get("bom") is None:
                info["bom"] = bom.bom
        self.emit({"type": "file", "file": bom.name, "event": "bom", "bom": bom.bom, "wall": bom.arrival.wall,
                   "time": fmt_wall(bom.arrival.wall)})

    def close_open_frames(self, wall: float, why: str = "") -> None:
        """Report frames that never got an END as partial and forget them."""
        for snap, frame in list(self.open_frames.items()):
            self.anomaly("partial_frame", f"frame {snap} has no END ({frame.lines} lines seen){why}", wall, snap_id=snap)
            self.finish_frame(frame, None)
        self.open_frames.clear()

    def reset_session(self, kind: str, wall: float) -> None:
        """A new debug.log session (game restart): open frames end as partial, per-session state starts over."""
        self.close_open_frames(wall, f"; debug.log was {kind}")
        self.done_ids.clear()
        self.seen_payloads.clear()
        self.session += 1

    def add_budget(self, name: str, arrival: Arrival, raw_len: int, is_curia: bool) -> None:
        if self.start_mono is None:
            self.start_mono = arrival.mono
        self.last_mono = arrival.mono
        kind = "curia" if is_curia else "other"
        self.budget_total[name][kind] += raw_len
        minute = int((arrival.mono - self.start_mono) // 60)
        bucket = self.budget_minutes[name].setdefault(minute, {"other": 0, "curia": 0})
        bucket[kind] += raw_len

    # ---- entry point -------------------------------------------------------------------------

    def ingest(self, tl: TailLines, label: str) -> None:
        if not tl.backlog:
            self.last_growth[tl.name] = tl.arrival.wall
        self.sizes.setdefault(tl.name, {})["last_size"] = tl.size
        for line in tl.lines:
            if line.crlf:
                self.crlf_lines[tl.name] += 1
            if tl.name == "debug.log":
                self.on_debug_line(tl, line, label)
            else:
                self.on_other_line(tl, line)

    def on_other_line(self, tl: TailLines, line: Line) -> None:
        is_hit = tl.name == "error.log" and self.mod_re.search(line.data) is not None
        if not tl.backlog:
            self.add_budget(tl.name, tl.arrival, line.raw_len, False)
        if utf8_error_offset(line.data) is not None:
            self.invalid_utf8_other[tl.name] += 1
        if is_hit:
            rec = {
                "type": "error_hit",
                "file": tl.name,
                "wall": tl.arrival.wall,
                "time": fmt_wall(tl.arrival.wall),
                "backlog": tl.backlog,
                "text": decode_replace(line.data[:2000]),
            }
            self.error_hits.append(rec)
            self.emit(rec)

    # ---- debug.log lines ---------------------------------------------------------------------

    def on_debug_line(self, tl: TailLines, line: Line, label: str) -> None:
        self.debug_seq += 1
        prefix = parse_prefix(line.data)
        if prefix is not None and not tl.backlog:
            engine_sod = (prefix.second_of_day + self.cfg.engine_offset_s) % DAY_SECONDS
            d = raw_difference(seconds_of_day(tl.arrival.wall), engine_sod)
            self.diff_floor_counts[int(d // 1)] += 1
        pos = find_marker(line.data)
        if not tl.backlog:
            self.add_budget(tl.name, tl.arrival, line.raw_len, pos >= 0)
        if pos < 0:
            self.noncuria_count += 1
            if len(line.data) > self.cfg.long_line_bytes:
                self.long_other_lines += 1
            if utf8_error_offset(line.data) is not None:
                self.invalid_utf8_other[tl.name] += 1
            return
        self.on_curia_line(tl, line, label, pos, prefix)

    def on_curia_line(self, tl: TailLines, line: Line, label: str, pos: int, prefix: Optional[EnginePrefix]) -> None:
        self.curia_count += 1
        payload_full = line.data[pos:]  # starts with the marker
        payload = payload_full[len(MARKER) :]
        wall = tl.arrival.wall
        # the terminator that was seen (none for a line emitted early at the partial-buffer cap)
        self.emit_raw(line.data + (b"" if line.capped else b"\r\n" if line.crlf else b"\n"))
        self.curia_lengths.append(len(line.data))
        self.curia_prefix_lengths.append(pos)
        lower = upper = None
        engine_sod: Optional[float] = None
        if prefix is None:
            self.curia_no_prefix += 1
        else:
            self.prefix_sources[f"[{prefix.level}][{prefix.source}]"] += 1
            engine_sod = (prefix.second_of_day + self.cfg.engine_offset_s) % DAY_SECONDS
            lower, upper = latency_bounds(seconds_of_day(wall), engine_sod)
        sample = Sample(label, wall, engine_sod, lower, upper, len(line.data), line.raw_len, line.reads, tl.backlog)
        self.samples.append(sample)
        if line.reads > 1:
            self.reads_per_line_gt1 += 1
        self.emit(
            {
                "type": "sample",
                "file": tl.name,
                "label": label,
                "backlog": tl.backlog,
                "arrival": fmt_wall(wall),
                "arrival_wall": round(wall, 3),
                "arrival_mono": round(tl.arrival.mono, 4),
                "engine_clock": prefix.clock if prefix else None,
                "engine_level": prefix.level if prefix else None,
                "engine_source": prefix.source if prefix else None,
                "lower_bound_s": None if lower is None else round(lower, 3),
                "upper_bound_s": None if upper is None else round(upper, 3),
                "line_bytes": len(line.data),
                "raw_bytes": line.raw_len,
                "prefix_bytes": pos,
                "payload_bytes": len(payload_full),
                "reads": line.reads,
                "crlf": line.crlf,
                "file_mtime": fmt_wall(tl.mtime),
                "file_offset": line.offset,
                "text": decode_replace(payload),
            }
        )
        self.check_curia_line(line, payload_full, pos, wall)
        self.track_frame(tl, line, label, payload, payload_full)

    def check_curia_line(self, line: Line, payload_full: bytes, pos: int, wall: float) -> None:
        if len(line.data) > self.cfg.long_line_bytes:
            self.anomaly(
                "long_line", f"CURIA line of {len(line.data)} bytes (limit {self.cfg.long_line_bytes})", wall,
                line_bytes=len(line.data),
            )
        if line.capped:
            self.anomaly("partial_buffer_cap", "line emitted early because it exceeded the partial-line cap", wall)
        err = utf8_error_offset(payload_full)
        if err is not None:
            self.anomaly(
                "invalid_utf8", f"invalid UTF-8 at payload offset {err}", wall, offset=err,
                text=decode_replace(payload_full[:300]),
            )
        odd = odd_bytes(payload_full)
        if odd:
            self.anomaly(
                "non_ascii_or_control", "non-ASCII or control bytes in a CURIA line (offsets from the marker: "
                + ", ".join(f"{o}:{b}" for o, b in odd) + ")", wall,
                offsets=[o for o, _ in odd], line_offsets=[pos + o for o, _ in odd],
                text=decode_replace(payload_full[:300]),
            )
        digest = hashlib.blake2b(payload_full, digest_size=12).digest()
        if digest in self.seen_payloads:
            self.anomaly("duplicate_line", "identical CURIA line seen more than once", wall,
                         text=decode_replace(payload_full[:200]))
        self.seen_payloads.add(digest)

    # ---- frames ------------------------------------------------------------------------------

    def track_frame(self, tl: TailLines, line: Line, label: str, payload: bytes, payload_full: bytes) -> None:
        wall = tl.arrival.wall
        fields = payload.split(b"|", 2)
        snap = decode_replace(fields[0]) if fields and fields[0] else "(none)"
        kind = fields[1] if len(fields) > 1 else b""
        rest = fields[2] if len(fields) > 2 else b""
        if len(fields) < 2 or not fields[0]:
            self.anomaly("malformed_marker_line", "CURIA line without snap_id and kind fields", wall,
                         text=decode_replace(payload_full[:200]))
        frame = self.open_frames.get(snap)
        if kind == b"BEGIN" and (frame is not None or snap in self.done_ids):
            self.anomaly("duplicate_snap_id", f"BEGIN for snap_id {snap} that was already seen", wall)
            if frame is not None:
                self.finish_frame(frame, "new BEGIN for the same snap_id before END")
                self.open_frames.pop(snap, None)
            frame = None
        elif frame is None and snap in self.done_ids:
            self.anomaly("line_after_end", f"line for finished frame {snap} (kind {decode_replace(kind)})", wall)
            return
        if frame is None:
            frame = Frame(snap, label, wall, wall, self.debug_seq, self.debug_seq, self.noncuria_count, self.noncuria_count,
                          session=self.session)
            self.open_frames[snap] = frame
        frame.last_wall = wall
        frame.last_seq = self.debug_seq
        frame.noncuria_last = self.noncuria_count
        frame.lines += 1
        frame.payload_bytes += len(payload_full)
        frame.raw_bytes += line.raw_len
        frame.longest = max(frame.longest, len(line.data))
        frame.backlog = frame.backlog or tl.backlog
        if len(frame.kinds) < 64:
            frame.kinds.append(decode_replace(kind[:16]))
        if kind == b"BEGIN":
            frame.has_begin = True
        elif kind == b"END":
            frame.has_end = True
            try:
                frame.declared = int(rest.split(b"|", 1)[0])
            except ValueError:
                self.anomaly("bad_end_count", "END line count is not an integer", wall,
                             text=decode_replace(rest[:60]))
        else:
            frame.body_lines += 1
        if frame.has_end:
            self.finish_frame(frame, None)
            self.open_frames.pop(snap, None)

    def finish_frame(self, frame: Frame, reason: Optional[str]) -> None:
        problems: List[str] = []
        if reason:
            problems.append(reason)
        if not frame.has_begin:
            problems.append("no BEGIN")
        if not frame.has_end:
            problems.append("no END")
        match_all = frame.declared is not None and frame.declared == frame.lines
        match_body = frame.declared is not None and frame.declared == frame.body_lines
        convention_match = match_all if self.cfg.count_convention == "all" else match_body
        if frame.has_end and not convention_match:
            problems.append(
                f"END states {frame.declared} lines, saw {frame.lines} (body only {frame.body_lines}); "
                f"convention '{self.cfg.count_convention}' not met"
            )
        interleaved_total = (frame.last_seq - frame.first_seq + 1) - frame.lines
        interleaved_noncuria = frame.noncuria_last - frame.noncuria_first
        complete = frame.has_begin and frame.has_end and convention_match and not reason
        rec = {
            "type": "frame",
            "snap_id": frame.snap_id,
            "complete": complete,
            "problems": problems,
            "label": frame.label,
            "lines": frame.lines,
            "body_lines": frame.body_lines,
            "declared_count": frame.declared,
            "declared_matches_all_lines": match_all,
            "declared_matches_body_lines": match_body,
            "payload_bytes": frame.payload_bytes,
            "raw_bytes_with_prefix": frame.raw_bytes,
            "longest_line_bytes": frame.longest,
            "first_arrival": fmt_wall(frame.first_wall),
            "last_arrival": fmt_wall(frame.last_wall),
            "span_s": round(frame.last_wall - frame.first_wall, 3),
            "interleaved_noncuria_lines": interleaved_noncuria,
            "interleaved_other_frame_lines": max(0, interleaved_total - interleaved_noncuria),
            "backlog": frame.backlog,
            "session": frame.session,
            "kinds": frame.kinds,
        }
        self.frames_done.append(rec)
        self.done_ids.add(frame.snap_id)
        self.emit(rec)
        if frame.has_end and not convention_match:
            self.anomaly("frame_count_mismatch", problems[-1], frame.last_wall, snap_id=frame.snap_id)

    def finalize(self, wall: float) -> None:
        """Close frames that never got an END (partial frames)."""
        self.close_open_frames(wall)
        self.emit(
            {
                "type": "budget",
                "minutes": {
                    name: {str(m): b for m, b in sorted(buckets.items())}
                    for name, buckets in self.budget_minutes.items()
                },
                "totals": {name: dict(t) for name, t in self.budget_total.items()},
            }
        )

    # ---- derived numbers ---------------------------------------------------------------------

    def samples_by_label(self) -> "collections.OrderedDict[str, List[Sample]]":
        out: "collections.OrderedDict[str, List[Sample]]" = collections.OrderedDict()
        for s in self.samples:
            if not s.backlog:
                out.setdefault(s.label, []).append(s)
        return out

    def clock_offset_info(self) -> Optional[Dict[str, Any]]:
        """Distribution of floor(arrival - engine second) over every prefixed line (not only CURIA)."""
        total = sum(self.diff_floor_counts.values())
        if not total:
            return None
        keys = sorted(self.diff_floor_counts)
        cum = 0
        median = keys[-1]
        for k in keys:
            cum += self.diff_floor_counts[k]
            if cum * 2 >= total:
                median = k
                break
        return {"count": total, "min": keys[0], "max": keys[-1], "median": median}

    def active_window_s(self) -> float:
        """Seconds from the first appended byte to the latest one: the time base of the byte rates."""
        if self.start_mono is None or self.last_mono is None:
            return 0.0
        return max(0.0, self.last_mono - self.start_mono)


# --------------------------------------------------------------------------------------------
# Summary text
# --------------------------------------------------------------------------------------------


def build_summary(an: Analysis, meta: Dict[str, Any]) -> str:
    out: List[str] = []
    add = out.append
    add("Curia M3 log watcher summary")
    add("=" * 60)
    add(f"tool version {TOOL_VERSION}; python {platform.python_version()}; {platform.platform()}")
    add(f"watch started {meta.get('started')}; ended {meta.get('ended')}; elapsed {meta.get('elapsed_s', 0):.1f} s")
    add(f"log folder: {meta.get('log_dir')}")
    add(
        f"poll interval {meta.get('interval_ms')} ms; from-start {meta.get('from_start')}; "
        f"END count convention '{an.cfg.count_convention}'; engine clock offset applied {an.cfg.engine_offset_s:+.3f} s "
        f"(added to the engine clock); mod pattern '{an.cfg.mod_pattern}'"
    )
    add("")
    add_files_section(an, add)
    add_latency_section(an, add)
    add_format_section(an, add)
    add_frames_section(an, add)
    add_truncation_section(an, add)
    add_error_section(an, add)
    add_budget_section(an, add, meta.get("elapsed_s", 0.0))
    add_clock_section(an, add)
    add_anomaly_section(an, add)
    add("")
    add("How to read the bounds: the engine stamps whole seconds, so each sample only brackets its latency")
    add("(lower = arrival - (engine second + 1), upper = arrival - engine second). Arrival is the moment the")
    add(f"poll saw the bytes, so up to one poll interval ({meta.get('interval_ms')} ms) of observation delay is included.")
    add("The engine timestamp is taken as the time the debug_log call ran; the button click time is not measured.")
    return "\n".join(out) + "\n"


def add_files_section(an: Analysis, add: Callable[[str], None]) -> None:
    add("1. Files")
    for name in an.cfg.files:
        info = an.sizes.get(name)
        if not info:
            add(f"  {name}: never seen (waiting for {name})")
            continue
        last = an.last_growth.get(name)
        add(
            f"  {name}: first size seen {fmt_bytes(info.get('first_size', 0))}, last size seen {fmt_bytes(info.get('last_size', info.get('first_size', 0)))}"
            f", UTF-8 BOM at start {info.get('bom')}, last growth {fmt_wall(last) if last else 'none seen'}"
            f", CRLF-terminated lines {an.crlf_lines.get(name, 0)}"
        )
    add("")


def add_latency_section(an: Analysis, add: Callable[[str], None]) -> None:
    add("2. Flush latency bounds per label (T3)")
    groups = an.samples_by_label()
    backlog = sum(1 for s in an.samples if s.backlog)
    if backlog:
        add(f"  ({backlog} CURIA line(s) read as backlog with --from-start are excluded from the statistics)")
    if not groups:
        add("  no CURIA1 samples")
        add("")
        return
    allsamples = [s for lst in groups.values() for s in lst]
    rows = list(groups.items())
    if len(groups) > 1:
        rows.append(("(all labels)", allsamples))
    for label, lst in rows:
        withb = [s for s in lst if s.lower is not None and s.upper is not None]
        lows = [s.lower for s in withb if s.lower is not None]
        ups = [s.upper for s in withb if s.upper is not None]
        add(f"  label '{label}': {len(lst)} sample(s), {len(withb)} with an engine timestamp")
        if not withb:
            continue
        lmin, lmed, lmax = mmm(lows)
        umin, umed, umax = mmm(ups)
        after = sum(1 for v in lows if v > 0)
        multi = sum(1 for s in lst if s.reads > 1)
        add(f"    lower bound s (min / median / max): {fmt_s(lmin)} / {fmt_s(lmed)} / {fmt_s(lmax)}")
        add(f"    upper bound s (min / median / max): {fmt_s(umin)} / {fmt_s(umed)} / {fmt_s(umax)}")
        add(f"    samples whose arrival was after the next whole second (lower bound > 0): {after} of {len(withb)}")
        add(f"    lines that needed more than one read to arrive complete: {multi}")
        add(f"    what the bounds permit: every one of these samples was seen {umax:.3f} s or less after the start of its engine second;")
        if lmax is not None and lmax > 0:
            add(f"      at least one sample was seen at least {lmax:.3f} s after its engine second ended (so its latency was not zero);")
        else:
            add("      no sample is proven to have taken any time (largest lower bound is not above zero), so zero latency is not excluded for any of them;")
        add(f"      at least one sample was seen within {umin:.3f} s of the start of its engine second.")
    add("")


def add_format_section(an: Analysis, add: Callable[[str], None]) -> None:
    add("3. CURIA line format, length and charset (T4)")
    if not an.curia_count:
        add("  no CURIA1 lines")
        add("")
        return
    lmin, lmed, lmax = mmm([float(x) for x in an.curia_lengths])
    pmin, pmed, pmax = mmm([float(x) for x in an.curia_prefix_lengths])
    add(f"  CURIA lines seen: {an.curia_count} (without a parseable engine prefix: {an.curia_no_prefix})")
    add(f"  line length bytes, terminator excluded (min / median / max): {lmin:.0f} / {lmed:.0f} / {lmax:.0f}")
    add(f"  bytes before the marker, i.e. engine prefix (min / median / max): {pmin:.0f} / {pmed:.0f} / {pmax:.0f}")
    add("  engine prefix shapes seen (level and source:line), count:")
    for shape, n in an.prefix_sources.most_common(8):
        add(f"    {shape}  x{n}")
    add(f"  non-CURIA debug.log lines: {an.noncuria_count}; of them over {an.cfg.long_line_bytes} bytes: {an.long_other_lines}")
    for name, n in sorted(an.invalid_utf8_other.items()):
        add(f"  non-CURIA lines with invalid UTF-8 in {name}: {n}")
    add("")


def add_frames_section(an: Analysis, add: Callable[[str], None]) -> None:
    add("4. Frames (BEGIN .. END by snap_id)")
    frames = an.frames_done
    if not frames:
        add("  none")
        add("")
        return
    ok = [f for f in frames if f["complete"]]
    add(f"  {len(frames)} frame(s): {len(ok)} complete, {len(frames) - len(ok)} with problems")
    all_n = sum(1 for f in frames if f["declared_matches_all_lines"])
    body_n = sum(1 for f in frames if f["declared_matches_body_lines"])
    add(f"  END count equals all lines of the frame (BEGIN and END included) in {all_n} frame(s); equals the body lines only in {body_n}")
    if ok:
        pmin, pmed, pmax = mmm([float(f["payload_bytes"]) for f in ok])
        rmin, rmed, rmax = mmm([float(f["raw_bytes_with_prefix"]) for f in ok])
        add(f"  complete frames, CURIA text bytes per export (min / median / max): {pmin:.0f} / {pmed:.0f} / {pmax:.0f}")
        add(f"  complete frames, bytes in the file incl. engine prefix (min / median / max): {rmin:.0f} / {rmed:.0f} / {rmax:.0f}")
    add("  snap_id | lines | END says | payload B | with prefix B | longest | span s | interleaved (other/frames) | label | status")
    for f in frames[:40]:
        status = "ok" if f["complete"] else "; ".join(f["problems"])
        add(
            f"  {f['snap_id']} | {f['lines']} | {f['declared_count']} | {f['payload_bytes']} | {f['raw_bytes_with_prefix']} | "
            f"{f['longest_line_bytes']} | {f['span_s']:.3f} | {f['interleaved_noncuria_lines']}/{f['interleaved_other_frame_lines']} | "
            f"{f['label']} | {status}"
        )
    if len(frames) > 40:
        add(f"  ... {len(frames) - 40} more frame(s) in the .jsonl file")
    add("")


def add_truncation_section(an: Analysis, add: Callable[[str], None]) -> None:
    add("5. Truncation, re-creation and disappearance (T5)")
    events = [e for e in an.file_events if e["type"] == "truncation" or e["event"] == "appeared"]
    if not any(e["type"] == "truncation" for e in events):
        add("  no truncation, re-creation or disappearance observed")
    for e in events:  # chronological, as recorded
        if e["event"] == "appeared":
            add(f"  {e['time']} {e['file']}: appeared after the watcher started, first size {e['new_size']} B, UTF-8 BOM {e['bom']}")
        else:
            add(
                f"  {e['time']} {e['file']}: {e['event']}, old size {e['old_size']} B, new size {e['new_size']} B"
                f", unterminated bytes discarded {e['discarded_partial_bytes']}, UTF-8 BOM at start {e['bom']}"
            )
    add("")


def add_error_section(an: Analysis, add: Callable[[str], None]) -> None:
    add(f"6. error.log lines matching the mod pattern '{an.cfg.mod_pattern}' (case-insensitive regex)")
    if not an.error_hits:
        add("  none")
    for hit in an.error_hits[:50]:
        add(f"  {hit['time']}: {hit['text'][:300]}")
    if len(an.error_hits) > 50:
        add(f"  ... {len(an.error_hits) - 50} more in the .jsonl file")
    add("")


def add_budget_section(an: Analysis, add: Callable[[str], None], elapsed_s: float) -> None:
    add("7. Byte budget (bytes appended while watching; content present at attach is not counted)")
    window_s = an.active_window_s()
    minutes = window_s / 60.0
    add(f"  watch elapsed {elapsed_s / 60.0:.1f} min; active window (first appended byte to the latest one) {minutes:.1f} min.")
    add("  The rates are the totals divided by the active window, so idle time before the game wrote anything is not counted; the")
    add("  per-minute buckets start at the same first appended byte (measured, not a forecast; any start-up burst is included).")
    add("  'per hour' is shown only for active windows of 10 minutes or more.")
    add("  file | excl. CURIA | CURIA lines | incl. CURIA | incl. CURIA per min | per hour | largest minute (excl. / CURIA) | minutes with growth")
    for name in an.cfg.files:
        tot = an.budget_total.get(name, {"other": 0, "curia": 0})
        both = tot["other"] + tot["curia"]
        per_min = fmt_bytes(both / minutes) if minutes > 0 else "n/a (no active window)"
        per_hour = fmt_bytes(both * 60.0 / minutes) if minutes >= 10 else "n/a (window under 10 min)"
        buckets = an.budget_minutes.get(name, {})
        mo = max((b["other"] for b in buckets.values()), default=0)
        mc = max((b["curia"] for b in buckets.values()), default=0)
        add(
            f"  {name} | {fmt_bytes(tot['other'])} | {fmt_bytes(tot['curia'])} | {fmt_bytes(both)} | {per_min} | {per_hour} | "
            f"{fmt_bytes(mo)} / {fmt_bytes(mc)} | {len(buckets)}"
        )
    add("  (the full per-minute table is in the 'budget' event at the end of the .jsonl file)")
    add("")


def add_clock_section(an: Analysis, add: Callable[[str], None]) -> None:
    add("8. Engine clock versus arrival clock (all prefixed debug.log lines)")
    info = an.clock_offset_info()
    if info is None:
        add("  no prefixed lines seen")
        add("")
        return
    add(
        f"  floor(arrival - engine second) over {info['count']} line(s): min {info['min']} s, median {info['median']} s, max {info['max']} s"
    )
    if info["min"] < 0:
        add(f"  apparent offset: some lines arrived BEFORE their engine second began (min {info['min']} s), which two agreeing clocks cannot do:")
        add("  the engine clock looks ahead of this machine's clock by at least that much (timezone, clock skew, or a day boundary).")
        add("  The latency bounds above contain this offset; --engine-offset-s is ADDED to the engine clock before the bounds are computed,")
        add("  so an engine clock that runs ahead needs a negative value (for example -3600 for one hour ahead).")
    elif info["min"] > 5:
        add(f"  apparent offset: even the earliest lines arrived {info['min']} s or more after their engine second. A constant clock offset")
        add("  (for example a timezone or skew) and a genuine delay cannot be told apart from these numbers alone.")
    else:
        add("  no constant offset is apparent (the earliest lines arrive within a few seconds of their engine second).")
    add("")


def add_anomaly_section(an: Analysis, add: Callable[[str], None]) -> None:
    add("9. Anomalies")
    if not an.anomalies:
        add("  none")
        add("")
        return
    for kind, n in an.anomaly_counts.most_common():
        add(f"  {kind}: {n}")
    for a in an.anomalies[:30]:
        extra = f" text={a['text'][:120]!r}" if a.get("text") else ""
        add(f"    {a['time']} {a['kind']}: {a['detail']}{extra}")
    if len(an.anomalies) > 30:
        add(f"    ... {len(an.anomalies) - 30} more in the .jsonl file")
    add("")


# --------------------------------------------------------------------------------------------
# Output files
# --------------------------------------------------------------------------------------------


class Recorder:
    """Writes the jsonl event log, the raw CURIA lines and the summary into the output folder."""

    def __init__(self, out_dir: str, stamp: str):
        os.makedirs(out_dir, exist_ok=True)
        self.jsonl_path = os.path.join(out_dir, f"watch-{stamp}.jsonl")
        self.raw_path = os.path.join(out_dir, f"raw-curia-{stamp}.txt")
        self.summary_path = os.path.join(out_dir, f"summary-{stamp}.txt")
        self._jsonl = open(self.jsonl_path, "w", encoding="utf-8")
        self._raw = open(self.raw_path, "wb")

    def event(self, rec: Dict[str, Any]) -> None:
        self._jsonl.write(json.dumps(rec, ensure_ascii=True, separators=(",", ":")) + "\n")
        self._jsonl.flush()

    def raw(self, data: bytes) -> None:
        """One CURIA line including the terminator that was seen (LF or CRLF)."""
        self._raw.write(data)
        self._raw.flush()

    def write_summary(self, text: str) -> None:
        with open(self.summary_path, "w", encoding="utf-8") as f:
            f.write(text)

    def close(self) -> None:
        self._jsonl.close()
        self._raw.close()


# --------------------------------------------------------------------------------------------
# The watcher
# --------------------------------------------------------------------------------------------


class Watcher:
    """Polls the log files, feeds the analysis, writes events, prints progress."""

    def __init__(
        self,
        cfg: Config,
        clock: Optional[Clock] = None,
        labels: Optional[LabelState] = None,
        recorder: Optional[Recorder] = None,
        echo: Optional[Callable[[str], None]] = None,
    ):
        self.cfg = cfg
        self.clock = clock or Clock()
        self.labels = labels or LabelState(self.clock, cfg.initial_label)
        self.recorder = recorder
        self.echo = echo or (lambda text: print(text, flush=True))
        self.analysis = Analysis(cfg, self._emit, self._emit_raw)
        self.tailers = {
            name: FileTailer(os.path.join(cfg.log_dir, name), name, self.clock, cfg.from_start, cfg.max_partial_bytes)
            for name in cfg.files
        }
        self.started_wall = self.clock.wall()
        self.started_mono = self.clock.mono()
        self.next_report = self.started_mono + cfg.report_every if cfg.report_every > 0 else None
        self.last_budget_snapshot: Dict[str, int] = {}
        self.last_report_mono = self.started_mono
        self._emit({"type": "start", "wall": self.started_wall, "time": fmt_wall(self.started_wall),
                    "log_dir": cfg.log_dir, "interval_ms": cfg.interval_ms, "from_start": cfg.from_start,
                    "count_convention": cfg.count_convention, "tool_version": TOOL_VERSION})

    # ---- sinks -------------------------------------------------------------------------------

    def _emit(self, rec: Dict[str, Any]) -> None:
        if self.recorder is not None:
            self.recorder.event(rec)

    def _emit_raw(self, data: bytes) -> None:
        if self.recorder is not None:
            self.recorder.raw(data)

    def say(self, text: str) -> None:
        if not self.cfg.quiet:
            self.echo(text)

    # ---- polling -----------------------------------------------------------------------------

    def poll_once(self) -> None:
        self.drain_labels()
        for name, tailer in self.tailers.items():
            for ev in tailer.poll():
                if isinstance(ev, TailFileEvent):
                    self.on_file_event(ev)
                elif isinstance(ev, TailBom):
                    self.analysis.note_bom(ev)
                else:
                    self.on_lines(ev)

    def drain_labels(self) -> None:
        for ev in self.labels.drain():
            ev = dict(ev)
            ev["time"] = fmt_wall(ev["wall"])
            self._emit(ev)
            if ev["type"] == "label":
                self.echo(f"label -> {ev['label']}")
            else:
                self.echo(f"note recorded: {ev['text']}")

    def on_file_event(self, ev: TailFileEvent) -> None:
        if ev.kind == "waiting":
            self.echo(f"waiting for {ev.name} in {self.cfg.log_dir}")
            self._emit({"type": "file", "file": ev.name, "event": "waiting", "wall": ev.arrival.wall,
                        "time": fmt_wall(ev.arrival.wall)})
            return
        self.analysis.note_file_event(ev)
        self.echo(f"{fmt_clock(ev.arrival.wall)} {ev.name}: {ev.kind} (old size {ev.old_size} B, new size {ev.new_size} B)")

    def on_lines(self, tl: TailLines) -> None:
        label = self.labels.current()
        before = len(self.analysis.samples)
        self.analysis.ingest(tl, label)
        for s in self.analysis.samples[before:]:
            if s.lower is None:
                self.say(f"{fmt_clock(s.arrival_wall)} [{label}] CURIA1 line without engine prefix ({s.line_bytes} B)")
            else:
                self.say(
                    f"{fmt_clock(s.arrival_wall)} [{label}] CURIA1 {s.line_bytes} B, bounds {s.lower:+.3f} .. {s.upper:+.3f} s"
                    + (" (backlog)" if s.backlog else "")
                )

    def maybe_report(self) -> None:
        if self.next_report is None:
            return
        now = self.clock.mono()
        if now < self.next_report:
            return
        self.next_report = now + self.cfg.report_every
        elapsed_min = max((now - self.last_report_mono) / 60.0, 1e-9)
        self.last_report_mono = now
        parts: List[str] = []
        status: Dict[str, Any] = {"type": "status", "wall": self.clock.wall(), "label": self.labels.current()}
        for name in self.cfg.files:
            t = self.analysis.budget_total.get(name, {"other": 0, "curia": 0})
            total = t["other"] + t["curia"]
            delta = total - self.last_budget_snapshot.get(name, 0)
            self.last_budget_snapshot[name] = total
            size = self.analysis.sizes.get(name, {}).get("last_size")
            status[name] = {"size": size, "appended_total": total, "curia_total": t["curia"], "per_min": round(delta / elapsed_min)}
            parts.append(f"{name} {fmt_bytes(size) if size is not None else 'absent'} (+{fmt_bytes(delta / elapsed_min)}/min, CURIA {fmt_bytes(t['curia'])})")
        status["samples"] = len(self.analysis.samples)
        status["frames"] = len(self.analysis.frames_done)
        self._emit(status)
        self.echo(
            f"{fmt_clock(self.clock.wall())} [{self.labels.current()}] " + "; ".join(parts)
            + f"; samples {len(self.analysis.samples)}, frames {len(self.analysis.frames_done)}"
        )

    def run(self, stop: Optional[threading.Event] = None) -> None:
        """Poll until the duration ends or the stop event is set; on KeyboardInterrupt, poll once more and re-raise."""
        interval = self.cfg.interval_ms / 1000.0
        end = self.started_mono + self.cfg.duration if self.cfg.duration > 0 else None
        try:
            while True:
                self.poll_once()
                self.maybe_report()
                if stop is not None and stop.is_set():
                    break
                if end is not None and self.clock.mono() >= end:
                    break
                self.clock.sleep(interval)
        except KeyboardInterrupt:
            self.poll_once()  # bytes that arrived since the last poll
            raise
        self.poll_once()

    def finish(self) -> str:
        """Close open frames, build the summary, save it, return the text."""
        ended = self.clock.wall()
        self.drain_labels()
        self.analysis.finalize(ended)
        meta = {
            "started": fmt_wall(self.started_wall),
            "ended": fmt_wall(ended),
            "elapsed_s": self.clock.mono() - self.started_mono,
            "log_dir": self.cfg.log_dir,
            "interval_ms": self.cfg.interval_ms,
            "from_start": self.cfg.from_start,
        }
        text = build_summary(self.analysis, meta)
        self._emit({"type": "end", "wall": ended, "time": fmt_wall(ended)})
        if self.recorder is not None:
            self.recorder.write_summary(text)
            self.recorder.close()
        return text


# --------------------------------------------------------------------------------------------
# Self-test: a simulated writer and the real watcher, no CK3
# --------------------------------------------------------------------------------------------


def engine_line(wall: float, text: str, source: str = "jomini_effect_impl.cpp:450") -> bytes:
    """A synthetic engine-style line stamped with the local whole second of `wall`."""
    t = time.localtime(wall)
    prefix = f"[{t.tm_hour:02d}:{t.tm_min:02d}:{t.tm_sec:02d}][D][{source}]"
    return (f"{prefix}: file: common/scripted_effects/x.txt line: 12 (effect): {text}\n").encode("utf-8")


def run_selftest(echo: Callable[[str], None] = print, tolerance_s: float = 0.25) -> bool:
    """Simulate a CK3-like writer in a temp folder and check what the watcher measured."""
    tmp = tempfile.mkdtemp(prefix="curia-selftest-")
    log_dir = os.path.join(tmp, "logs")
    os.makedirs(log_dir)
    out_dir = os.path.join(tmp, "out")
    debug_path = os.path.join(log_dir, "debug.log")
    cfg = Config(log_dir=log_dir, out_dir=out_dir, interval_ms=5.0, report_every=0.0, use_stdin=False, quiet=True)
    clock = Clock()
    writes: Dict[str, float] = {}
    problems: List[str] = []

    def check(ok: bool, what: str) -> None:
        echo(("PASS  " if ok else "FAIL  ") + what)
        if not ok:
            problems.append(what)

    # Each line's engine second is taken from the same instant the writer records as its write time.
    steps: List[Tuple[float, str, str]] = [
        (0.05, "CURIA1|s1|BEGIN|1066.9.15|1", "s1b"),
        (0.12, "CURIA1|s1|FIN|375|12", "s1f"),
        (0.30, "CURIA1|s1|NAME|Zo\u00eb of Aq\u00fcitaine", "s1n"),
        (0.08, "CURIA1|s1|END|4", "s1e"),
        (0.10, "CURIA1|s2|LONG|" + "x" * 8000, "s2l"),
    ]
    recorder = Recorder(out_dir, "selftest")
    watcher = Watcher(cfg, clock, LabelState(clock, "selftest"), recorder, echo=lambda _t: None)
    stop = threading.Event()

    def writer_main() -> None:
        with open(debug_path, "ab", buffering=0) as f:
            for delay, text, tag in steps:
                time.sleep(delay)
                stamp = time.time()
                f.write(engine_line(stamp, text))
                writes[tag] = stamp
            # a line delivered in two writes (partial line)
            time.sleep(0.05)
            stamp = time.time()
            full = engine_line(stamp, "CURIA1|s3|PART|half and half")
            f.write(full[:40])
            time.sleep(0.06)
            stamp2 = time.time()
            f.write(full[40:])
            writes["part"] = stamp2
            writes["part_engine"] = stamp
            time.sleep(0.1)
            # simulate a launch: the same file truncated, then new content
            f.truncate(0)
            f.seek(0)
        time.sleep(0.05)
        with open(debug_path, "wb", buffering=0) as f:
            stamp = time.time()
            f.write(engine_line(stamp, "CURIA1|s4|BEGIN|1066.9.16|1"))
            writes["s4b"] = stamp
        time.sleep(0.1)
        stop.set()

    thread = threading.Thread(target=writer_main)
    thread.start()
    watcher.run(stop)
    thread.join()
    text = watcher.finish()

    samples = list(watcher.analysis.samples)
    check(len(samples) == 7, f"seven CURIA samples seen (got {len(samples)})")
    slack = cfg.interval_ms / 1000.0 + tolerance_s
    tags = ["s1b", "s1f", "s1n", "s1e", "s2l", "part", "s4b"]
    for tag, s in zip(tags, samples):
        wrote = writes.get(tag)
        if wrote is None:
            check(False, f"writer recorded {tag}")
            continue
        delay = s.arrival_wall - wrote
        check(-0.001 <= delay <= slack, f"{tag}: arrival within {slack:.3f} s of the write (measured {delay:.4f} s)")
        engine_stamp = writes.get("part_engine") if tag == "part" else wrote
        if s.lower is not None and s.upper is not None and engine_stamp is not None:
            true_latency = s.arrival_wall - engine_stamp
            check(s.lower - 0.002 <= true_latency <= s.upper + 0.002, f"{tag}: true latency {true_latency:.3f} s inside the bounds {s.lower:.3f} .. {s.upper:.3f}")
    check(any(s.reads > 1 for s in samples), "a line written in two pieces was assembled across reads")
    check(any(s.line_bytes > 8000 for s in samples), "the 8,000-character line survived whole")
    frames = {f["snap_id"]: f for f in watcher.analysis.frames_done}
    check("s1" in frames and frames["s1"]["complete"], "frame s1 is complete (END count matches)")
    check(any(a["kind"] == "non_ascii_or_control" for a in watcher.analysis.anomalies), "the non-ASCII name was reported")
    check(any(e["type"] == "truncation" and e["event"] == "truncated" for e in watcher.analysis.file_events), "the truncation was detected")
    check("Flush latency bounds" in text and "Byte budget" in text, "the summary text has its sections")
    check(os.path.exists(recorder.summary_path), "the summary file was saved")
    shutil.rmtree(tmp, ignore_errors=True)
    echo("SELFTEST_OK" if not problems else f"SELFTEST_FAILED ({len(problems)} problem(s))")
    return not problems


# --------------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------------


def default_log_dir() -> str:
    return os.path.join(os.path.expanduser("~"), "Documents", "Paradox Interactive", "Crusader Kings III", "logs")


def default_out_dir() -> str:
    return os.path.join(os.path.expanduser("~"), "curia_m3_results")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Measure CK3 debug.log behaviour for the Curia mod (read-only).")
    p.add_argument("--log-dir", default=default_log_dir(), help="CK3 logs folder (default: the macOS/Windows user folder)")
    p.add_argument("--out-dir", default=default_out_dir(), help="where results are written (default ~/curia_m3_results)")
    p.add_argument("--interval-ms", type=float, default=10.0, help="poll interval in milliseconds (default 10)")
    p.add_argument("--duration", type=float, default=0.0, help="stop after this many seconds (default: until Ctrl+C)")
    p.add_argument("--report-every", type=float, default=30.0, help="print a status line every N seconds (0 = off, default 30)")
    p.add_argument("--from-start", action="store_true", help="read content already in the files as backlog (excluded from latency and budget)")
    p.add_argument("--count-convention", choices=("all", "body"), default="all",
                   help="does the END line count include BEGIN and END ('all', default) or only the lines between ('body')")
    p.add_argument("--long-line-bytes", type=int, default=4000, help="report CURIA lines longer than this (default 4000)")
    p.add_argument("--engine-offset-s", type=float, default=0.0,
                   help="seconds added to the engine clock before computing bounds (negative when the engine clock runs ahead)")
    p.add_argument("--mod-pattern", default=DEFAULT_MOD_PATTERN,
                   help="case-insensitive regex for error.log lines that mention the mod (default 'curia[_/]')")
    p.add_argument("--label", default="none", help="initial label")
    p.add_argument("--no-stdin", action="store_true", help="do not read labels and notes from the terminal")
    p.add_argument("--quiet", action="store_true", help="do not print a line per CURIA sample")
    p.add_argument("--selftest", action="store_true", help="run the built-in scenarios against a temporary folder and exit")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.selftest:
        # the selftest uses real timing; one retry absorbs a stalled machine
        if run_selftest():
            return 0
        print("selftest failed; retrying once")
        return 0 if run_selftest() else 1
    try:
        re.compile(args.mod_pattern)
    except re.error as exc:
        parser.error(f"--mod-pattern is not a valid regular expression: {exc}")
    cfg = Config(
        log_dir=os.path.expanduser(args.log_dir),
        out_dir=os.path.expanduser(args.out_dir),
        interval_ms=max(1.0, args.interval_ms),
        duration=args.duration,
        report_every=args.report_every,
        from_start=args.from_start,
        count_convention=args.count_convention,
        long_line_bytes=args.long_line_bytes,
        engine_offset_s=args.engine_offset_s,
        mod_pattern=args.mod_pattern,
        use_stdin=not args.no_stdin,
        quiet=args.quiet,
        initial_label=args.label,
    )
    clock = Clock()
    stamp = datetime.datetime.fromtimestamp(clock.wall()).strftime("%Y%m%d-%H%M%S")
    recorder = Recorder(cfg.out_dir, stamp)
    labels = LabelState(clock, cfg.initial_label)
    watcher = Watcher(cfg, clock, labels, recorder)
    if cfg.use_stdin:
        start_label_reader(sys.stdin, labels)

    def on_stop_signal(_signum: int, _frame: Any) -> None:
        raise KeyboardInterrupt

    stop_signals = [signal.SIGTERM] + ([signal.SIGHUP] if hasattr(signal, "SIGHUP") else [])
    previous = {sig: signal.signal(sig, on_stop_signal) for sig in stop_signals}
    previous[signal.SIGINT] = signal.getsignal(signal.SIGINT)
    print(f"watching {cfg.log_dir} (poll {cfg.interval_ms:g} ms); results in {cfg.out_dir}")
    print("type a label and Enter to tag what the game is doing (paused, menu, fast, normal ...); '!text' stores a note; Ctrl+C ends.")
    failed = False
    try:
        watcher.run()
    except KeyboardInterrupt:
        print("\nstopping ...")
    except Exception:
        failed = True
        traceback.print_exc()
    finally:
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)  # a second signal must not cut the summary short
        text = watcher.finish()
        print(text)
        print(f"saved: {recorder.summary_path}\n       {recorder.jsonl_path}\n       {recorder.raw_path}")
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
