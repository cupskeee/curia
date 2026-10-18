#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turns Curia's app log (curia.log) into the tables of the M4 game sittings.

The owner's sheet (docs/spikes/m4-session-sheet.md) runs this on the app log of a sitting and sends
back only what it prints: numbers and the fixed words the app writes, never a name, a text or a key.
The app log itself holds no more than that (app/src/core/app_log.h), and this tool prints even less:
a word is printed only if it is one of the words the app is known to write in that field, anything
else shows as "other"; a number only if it is a number.

    python3 tools/m4/analyze_applog.py ~/curia_m4_results/sitting-a/applog
    python3 tools/m4/analyze_applog.py curia.log.1 curia.log --timeline

A folder is read as the app log's two generations, curia.log.1 then curia.log. A path is
required: the tool never reads an app log it was not given (a run without one is a usage error),
so that nothing reads the owner's own app log by accident.

Sections: the files and runs, the readings (frame_complete: game date, gold, income, how long until
the overlay was shown), the income across month boundaries (S3, per series of readings a few game
days apart and overall), the questions (times, tokens, cache, how they ended), the cache pairs
(criterion 4), the stops (criterion 3), bytes per hour while CK3 ran (S4, criterion 8: overall, and
play only, from the first reading after each launch of CK3), the overlay's evidence (criterion 7),
CK3 and the notices, and the key check (lines shaped like an API key: count and line numbers only).

Exit status: 0 done; 1 a line shaped like an API key was found (the numbers are still printed);
2 no app log could be read, a usage error, or an internal error (then nothing is printed on stdout
and the message on stderr is a fixed text).

Standard library only, Python 3.10 or later; safe to run with `python3 -I`. Tolerant on purpose: a
line that does not parse, an unknown event or a missing field is counted or shown as "-".
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import statistics
import sys
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

LINE_RE = re.compile(
    r"^(\d{4}-\d{2}-\d{2})T(\d{2}):(\d{2}):(\d{2})\.(\d{3})Z (debug|info|warn|error) ([A-Za-z0-9_.:-]{1,64})(.*)$")

# The words the app writes, per event and field (app/src/...). Anything else prints as "other".
BOOL = {"true", "false"}
FRONT = {"yes", "no", "unknown"}
WORDS: Dict[Tuple[str, str], set] = {
    ("overlay_shown", "kind"): {"panel", "window"},
    ("overlay_shown", "reason"): {"start", "game_click", "hotkey", "tray"},
    ("overlay_shown", "focus"): {"key", "click", "activate"},
    ("overlay_shown", "panel_key"): BOOL,
    ("overlay_shown", "app_active"): BOOL,
    ("overlay_shown", "ck3_front"): FRONT,
    ("overlay_hidden", "kind"): {"panel", "window"},
    ("overlay_hidden", "app_active"): BOOL,
    ("overlay_focus", "why"): {"focus_gained", "focus_lost", "first_key", "settled_show", "settled_hide",
                               "after_return", "settled"},
    ("overlay_focus", "kind"): {"panel", "window"},
    ("overlay_focus", "visible"): BOOL,
    ("overlay_focus", "panel_key"): BOOL,
    ("overlay_focus", "input_focus"): BOOL,
    ("overlay_focus", "app_active"): BOOL,
    ("overlay_focus", "ck3_front"): FRONT,
    ("overlay_keys", "after"): {"start", "hide"},
    ("overlay_keys", "returned"): {"not_needed", "none", "frontmost", "game", "remembered"},
    ("overlay_keys", "to"): {"ck3", "other"},
    ("overlay_keys", "accepted"): BOOL,
    ("overlay_placed", "basis"): {"game", "pointer", "primary"},
    ("overlay_placed", "game_window"): BOOL,
    ("overlay_policy", "before"): {"regular", "accessory", "prohibited", "unknown"},
    ("overlay_policy", "policy"): {"regular", "accessory", "prohibited", "unknown"},
    ("overlay_policy", "switched"): BOOL,
    ("overlay_opened", "source"): {"game_click", "hotkey", "tray", "refresh"},
    ("controls", "tray"): {"created", "unavailable", "skipped_game_front", "skipped_linux"},
    ("controls", "hotkey"): {"registered", "taken", "failed", "not_grabbed", "unsupported"},
    ("window_start", "window"): {"overlay", "plain"},
    ("window_start", "focus"): {"key", "click", "activate"},
    ("window_start", "configured"): BOOL,
    ("window_start", "log_found"): BOOL,
    ("dev_config", "key"): {"set", "unset", "unusable", "disabled"},
    ("dev_config", "model"): {"set", "unset", "unusable", "disabled"},
    ("provider_set", "effort"): {"default", "low", "medium", "high", "xhigh", "max"},
    ("provider_set", "thinking"): {"default", "adaptive", "between_tools", "disabled"},
    ("notice", "name"): {"None", "RestartCk3", "SilentSoft", "Ck3NotRunning", "Unknown"},
    ("ask_error", "code"): {"Auth", "RateLimit", "Overloaded", "Network", "Cancelled", "BadRequest", "Policy",
                            "Other", "Unknown"},
    ("chat_error", "code"): {"Auth", "RateLimit", "Overloaded", "Network", "Cancelled", "BadRequest", "Policy",
                             "Other", "Unknown"},
    ("ask_refused", "outcome"): {"Started", "EmptyText", "Busy", "NoProvider", "NothingToRetry", "Unknown"},
    ("ask_done", "max_tokens"): BOOL,
    ("net_done", "outcome"): {"finished", "cancelled", "shutdown", "refused", "unknown"},
    ("net_done", "conn"): {"none", "closed", "stream_reset", "released"},
    ("net_done", "http"): {"1.0", "1.1", "2", "3", "none"},
    ("ck3_running", "running"): BOOL,
    ("log_bytes", "why"): {"minute", "exit"},
    ("log_notice", "notice"): {"attached", "truncated", "missing", "unreadable", "recovered"},
}
# A word the server sends (stop_reason): printed if it looks like one.
SERVER_WORD_RE = re.compile(r"^[a-z_]{1,40}$")
INT_RE = re.compile(r"^-?\d{1,18}$")
DATE_RE = re.compile(r"^(\d{1,4})-(\d{1,2})-(\d{1,2})$")
# The app writes UTC times of this century; a line outside these years is not one of its lines.
YEARS = (2000, 2999)
# The game's calendar: 365 days, no leap years. Readings at most this many game days apart form a
# series (the steps of the monthly-tick test are one day each).
MONTH_DAYS = (31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)
SERIES_STEP_DAYS = 3

# Key shapes (criterion 6): an Anthropic key, and a long token that mixes upper case, lower case and
# digits, as a secret does (the app log's own words and numbers never do).
KEY_RE = re.compile(rb"sk-ant-[A-Za-z0-9_\-]{8,}")
TOKEN_RE = re.compile(rb"[A-Za-z0-9_\-+/=]{32,}")
REDACTED = b"[redacted]"

CACHE_WINDOW_MS = 5 * 60 * 1000
SHOWN_WITHIN_MS = 10 * 1000
HOUR_MS = 3600 * 1000
# log_bytes comes once a minute; a sample this long after CK3 stopped still counts for its run.
BYTES_GRACE_MS = 62 * 1000
# A truncation or a byte count this long before the first ck3_running=true of a stretch belongs to
# it: the game writes its log as soon as it starts, and the app polls CK3 once per tick (1 s; every
# 5 ticks on Linux) and reads the log every 250 ms, so CK3's first lines may be logged first.
LAUNCH_LEAD_MS = 6 * 1000
# A stretch in which CK3 was launched but no reading came: its play is counted from this long after
# the launch (the first Advisor click marks the loaded game otherwise).
LAUNCH_FALLBACK_MS = 10 * 60 * 1000
MIN_WINDOW_MINUTES_FOR_RATE = 10


# --- reading ---------------------------------------------------------------------------------------


class Line:
    __slots__ = ("ms", "level", "event", "fields", "source", "number")

    def __init__(self, ms: int, level: str, event: str, fields: Dict[str, str], source: int, number: int):
        self.ms = ms
        self.level = level
        self.event = event
        self.fields = fields
        self.source = source
        self.number = number

    def word(self, key: str) -> str:
        """The field as one of the app's known words, "other", or "-" when it is missing."""
        value = self.fields.get(key)
        if value is None:
            return "-"
        allowed = WORDS.get((self.event, key))
        if allowed is not None:
            return value if value in allowed else "other"
        if key == "stop_reason":
            return value if SERVER_WORD_RE.match(value) else "other"
        return "other"

    def num(self, key: str) -> Optional[int]:
        value = self.fields.get(key)
        if value is None or not INT_RE.match(value):
            return None
        return int(value)

    def flag(self, key: str) -> Optional[bool]:
        value = self.fields.get(key)
        return {"true": True, "false": False}.get(value) if value is not None else None


def parse_fields(rest: str) -> Dict[str, str]:
    """key=value pairs as AppLog writes them; a value with a blank, '=', a quote or a backslash is
    quoted with the quote and the backslash escaped. A token without '=' is skipped."""
    fields: Dict[str, str] = {}
    i, n = 0, len(rest)
    while i < n:
        while i < n and rest[i] == " ":
            i += 1
        if i >= n:
            break
        equal = rest.find("=", i)
        blank = rest.find(" ", i)
        if equal < 0 or (0 <= blank < equal):
            i = n if blank < 0 else blank
            continue
        key = rest[i:equal]
        i = equal + 1
        if i < n and rest[i] == '"':
            i += 1
            value = []
            while i < n and rest[i] != '"':
                if rest[i] == "\\" and i + 1 < n:
                    i += 1
                value.append(rest[i])
                i += 1
            i += 1
            fields[key] = "".join(value)
        else:
            end = rest.find(" ", i)
            end = n if end < 0 else end
            fields[key] = rest[i:end]
            i = end
    return fields


def parse_line(text: str, source: int = 0, number: int = 0) -> Optional[Line]:
    match = LINE_RE.match(text)
    if match is None:
        return None
    day, hours, minutes, seconds, millis, level, event, rest = match.groups()
    if not YEARS[0] <= int(day[:4]) <= YEARS[1]:
        return None
    try:
        date = dt.datetime.strptime(day, "%Y-%m-%d").replace(tzinfo=dt.timezone.utc)
    except ValueError:
        return None
    ms = int(date.timestamp() * 1000) + ((int(hours) * 60 + int(minutes)) * 60 + int(seconds)) * 1000 + int(millis)
    return Line(ms, level, event, parse_fields(rest), source, number)


class KeyHits:
    def __init__(self) -> None:
        self.key_shaped: List[Tuple[int, int]] = []
        self.redacted: List[Tuple[int, int]] = []


def scan_keys(raw: bytes, source: int, number: int, hits: KeyHits) -> None:
    if KEY_RE.search(raw):
        hits.key_shaped.append((source, number))
    else:
        for token in TOKEN_RE.findall(raw):
            text = token.decode("ascii")
            if any(c.isupper() for c in text) and any(c.islower() for c in text) and any(c.isdigit() for c in text):
                hits.key_shaped.append((source, number))
                break
    if REDACTED in raw:
        hits.redacted.append((source, number))


class LogFile:
    def __init__(self, label: str) -> None:
        self.label = label
        self.lines = 0
        self.parsed = 0
        self.other = 0
        self.readable = True
        self.exists = True

    def state(self) -> str:
        return "yes" if self.readable else ("no" if self.exists else "does not exist")


def read_logs(paths: Sequence[str]) -> Tuple[List[LogFile], List[Line], KeyHits]:
    files: List[LogFile] = []
    lines: List[Line] = []
    hits = KeyHits()
    for index, path in enumerate(paths):
        info = LogFile(os.path.basename(path) or path)
        files.append(info)
        try:
            with open(path, "rb") as handle:
                for number, raw in enumerate(handle, 1):
                    info.lines += 1
                    scan_keys(raw, index, number, hits)
                    line = parse_line(raw.decode("utf-8", errors="replace").rstrip("\r\n"), index, number)
                    if line is None:
                        info.other += 1
                    else:
                        info.parsed += 1
                        lines.append(line)
        except OSError:
            info.readable = False
            info.exists = os.path.lexists(path)
    # Stable by time: lines of one millisecond keep the order they were written in.
    lines.sort(key=lambda line: (line.ms, line.source, line.number))
    return files, lines, hits


def expand_paths(paths: Sequence[str]) -> List[str]:
    """The files to read: a folder gives the app log's generations in it, a file (or a path that
    does not exist, listed so that the Files section names it) itself."""
    out: List[str] = []
    for path in paths:
        if os.path.isdir(path):
            for name in ("curia.log.1", "curia.log"):
                candidate = os.path.join(path, name)
                if os.path.isfile(candidate):
                    out.append(candidate)
        else:
            out.append(path)
    return out


# --- the model of a log ------------------------------------------------------------------------------


class Run:
    def __init__(self, number: int, start_ms: int) -> None:
        self.number = number
        self.start_ms = start_ms
        self.end_ms = start_ms
        self.exited = False
        self.lines: List[Line] = []

    def first(self, event: str) -> Optional[Line]:
        return next((line for line in self.lines if line.event == event), None)

    def of(self, *events: str) -> List[Line]:
        return [line for line in self.lines if line.event in events]


# Lines that come once near the start of a run: the chat window's first line is dev_config, then
# provider_set (or not_configured) and window_start; the headless chat starts with provider_set.
PROVIDER_EVENTS = ("provider_set", "not_configured")


def starts_run(current: Run, line: Line) -> bool:
    if line.event == "dev_config":
        return True
    if line.event in PROVIDER_EVENTS:
        return current.exited or any(l.event in PROVIDER_EVENTS for l in current.lines)
    if line.event == "window_start":
        return current.exited or current.first("window_start") is not None
    return False


def split_runs(lines: Iterable[Line]) -> List[Run]:
    """A run is one start of Curia, from its first line (dev_config in the chat window, provider_set
    in the headless chat) to the next run's. Its log_bytes why=exit line marks a clean exit; a few
    lines may still follow it (a request stopped at exit ends on the net thread), and they stay with
    the run. Without a start line (a crash, an older build) the byte counts starting again from a
    lower value also start a new run."""
    runs: List[Run] = []
    current: Optional[Run] = None
    last_read = -1
    for line in lines:
        restart = current is None or starts_run(current, line)
        if not restart and line.event == "log_bytes" and not current.exited:  # type: ignore[union-attr]
            read = line.num("read")
            restart = read is not None and read < last_read
        if restart:
            current = Run(len(runs) + 1, line.ms)
            runs.append(current)
            last_read = -1
        assert current is not None
        current.lines.append(line)
        current.end_ms = line.ms
        if line.event == "log_bytes":
            read = line.num("read")
            if read is not None:
                last_read = read
            if line.fields.get("why") == "exit":
                current.exited = True
    return runs


class Ask:
    def __init__(self, run: Run, started: Line) -> None:
        self.run = run
        self.started = started
        self.first_token: Optional[Line] = None
        self.end: Optional[Line] = None  # ask_done, ask_error or ask_cancelled
        self.request: Optional[Line] = None  # chat_request
        self.provider_end: Optional[Line] = None  # chat_done or chat_error
        self.net: Optional[Line] = None  # net_done

    @property
    def gen(self) -> Optional[int]:
        return self.started.num("gen")

    def result(self) -> str:
        if self.end is None:
            return "no end"
        if self.end.event == "ask_done":
            return "cut off (max_tokens)" if self.end.flag("max_tokens") else "done"
        if self.end.event == "ask_cancelled":
            return "stopped"
        return "error " + self.end.word("code")


def collect_asks(run: Run) -> List[Ask]:
    """The asks of a run with their lines: by generation for the controller's lines; the provider's
    chat_request is the one written just before ask_started (the same call writes both, the request
    first), and its chat_done, chat_error and net_done follow in order (one request at a time: a
    new ask is refused while one is in flight, and a stopped one ends on the net thread within
    milliseconds)."""
    asks: List[Ask] = []
    by_gen: Dict[int, Ask] = {}
    unclaimed: Optional[Line] = None
    for line in run.lines:
        if line.event == "ask_started":
            ask = Ask(run, line)
            ask.request, unclaimed = unclaimed, None
            asks.append(ask)
            if ask.gen is not None:
                by_gen[ask.gen] = ask
        elif line.event in ("ask_first_token", "ask_done", "ask_error", "ask_cancelled"):
            gen = line.num("gen")
            ask = by_gen.get(gen) if gen is not None else None
            if ask is None:
                continue
            if line.event == "ask_first_token":
                ask.first_token = ask.first_token or line
            elif ask.end is None:
                ask.end = line
        elif line.event == "chat_request":
            latest = asks[-1] if asks else None
            if latest is not None and latest.request is None and line.ms - latest.started.ms <= 1000:
                latest.request = line
            else:
                unclaimed = line
        elif line.event in ("chat_done", "chat_error"):
            ask = next((a for a in asks if a.request is not None and a.provider_end is None), None)
            if ask is not None:
                ask.provider_end = line
        elif line.event == "net_done":
            ask = next((a for a in asks if a.request is not None and a.net is None), None)
            if ask is not None:
                ask.net = line
    return asks


# --- formatting --------------------------------------------------------------------------------------


class Clock:
    def __init__(self, utc: bool) -> None:
        self.utc = utc

    def stamp(self, ms: int) -> dt.datetime:
        moment = dt.datetime.fromtimestamp(ms / 1000.0, tz=dt.timezone.utc)
        return moment if self.utc else moment.astimezone()

    def time(self, ms: int) -> str:
        return self.stamp(ms).strftime("%H:%M:%S")

    def full(self, ms: int) -> str:
        return self.stamp(ms).strftime("%Y-%m-%d %H:%M")

    def zone(self) -> str:
        return "UTC" if self.utc else "local time"


def show(value: Any) -> str:
    if value is None:
        return "-"
    if isinstance(value, bool):
        return "yes" if value else "no"
    return str(value)


def table(header: Sequence[str], rows: Iterable[Sequence[Any]]) -> List[str]:
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    for row in rows:
        out.append("| " + " | ".join(show(cell) for cell in row) + " |")
    return out


def table_or_none(header: Sequence[str], rows: List[Sequence[Any]]) -> List[str]:
    return table(header, rows) if rows else ["None."]


def median(values: List[int]) -> Optional[int]:
    return int(statistics.median(values)) if values else None


def per_hour(amount: int, ms: int) -> Optional[int]:
    if ms < MIN_WINDOW_MINUTES_FOR_RATE * 60 * 1000:
        return None
    return int(round(amount * HOUR_MS / ms))


# --- the sections ------------------------------------------------------------------------------------


def section_files(files: List[LogFile], lines: List[Line], clock: Clock) -> List[str]:
    out = ["## Files", ""]
    out += table(("File", "Lines", "Parsed", "Not in the app log's format", "Readable"),
                 [(i + 1, f.lines, f.parsed, f.other, f.state()) for i, f in enumerate(files)])
    if lines:
        out += ["", "From %s to %s (%s)." % (clock.full(lines[0].ms), clock.full(lines[-1].ms), clock.zone())]
    return out


def section_runs(runs: List[Run], clock: Clock) -> List[str]:
    rows = []
    for run in runs:
        window = run.first("window_start")
        dev = run.first("dev_config")
        controls = run.first("controls")
        provider = run.first("provider_set")
        policy = run.first("overlay_policy")
        rows.append((run.number, clock.full(run.start_ms), clock.time(run.end_ms),
                     round((run.end_ms - run.start_ms) / 60000.0, 1),
                     window.word("window") if window else "-", window.word("focus") if window else "-",
                     dev.word("key") if dev else "-", dev.word("model") if dev else "-",
                     controls.word("tray") if controls else "-", controls.word("hotkey") if controls else "-",
                     policy.word("policy") if policy else "-",
                     provider.word("effort") if provider else "-", provider.word("thinking") if provider else "-",
                     provider.num("persona_version") if provider else None, run.exited))
    out = ["## Runs (one per start of Curia)", ""]
    out += table(("Run", "Start", "End", "Minutes", "Window", "Focus mode", "Key", "Model", "Tray", "Hot key",
                  "Activation policy", "Effort", "Thinking", "Persona", "Clean exit"), rows)
    return out


class Reading:
    def __init__(self, run: Run, line: Line) -> None:
        self.run = run
        self.line = line
        self.shown: Optional[Line] = None
        self.placed: Optional[Line] = None
        match = DATE_RE.match(line.fields.get("game_date", ""))
        date = (int(match.group(1)), int(match.group(2)), int(match.group(3))) if match else None
        # An impossible date (month 13, day 0) counts as none.
        self.date: Optional[Tuple[int, int, int]] = date if date is not None and game_day(date) is not None else None

    @property
    def gold(self) -> Optional[int]:
        return self.line.num("gold")

    @property
    def income(self) -> Optional[int]:
        return self.line.num("income_tenths")

    def date_text(self) -> str:
        return "%04d-%02d-%02d" % self.date if self.date else "-"

    def mode(self) -> str:
        return {0: "Advisor", 1: "Curia probes"}.get(self.line.num("mode"), "-")  # type: ignore[arg-type]


def collect_readings(runs: List[Run]) -> List[Reading]:
    """The readings, each with the overlay_shown of its game click (within 10 s; the run loop shows
    the overlay right after it handled the frame) and the overlay_placed just before that."""
    readings: List[Reading] = []
    for run in runs:
        pending: List[Reading] = []
        placed: Optional[Line] = None
        for line in run.lines:
            if line.event == "frame_complete":
                reading = Reading(run, line)
                readings.append(reading)
                pending.append(reading)
            elif line.event == "overlay_placed":
                placed = line
            elif line.event == "overlay_shown" and line.fields.get("reason") == "game_click":
                for reading in pending:
                    if line.ms - reading.line.ms <= SHOWN_WITHIN_MS:
                        reading.shown = line
                        if placed is not None and placed.ms >= reading.line.ms:
                            reading.placed = placed
                pending = []
    return readings


def section_readings(readings: List[Reading], clock: Clock) -> List[str]:
    out = ["## Readings (frame_complete; criterion 1, the app's part)", ""]
    if not readings:
        return out + ["No reading in this log."]
    rows = []
    for i, r in enumerate(readings, 1):
        after = r.shown.ms - r.line.ms if r.shown else None
        rows.append((i, r.run.number, clock.time(r.line.ms), r.line.num("reading"), r.line.num("snap"), r.mode(),
                     r.date_text(), r.gold, r.income, r.line.num("records"), after,
                     r.placed.word("basis") if r.placed else "-"))
    out += table(("#", "Run", "Time", "Reading", "Snap", "Button", "Game date", "Gold", "Income (tenths)",
                  "Records", "Overlay shown after (ms)", "Placed by"), rows)
    shown = [r.shown.ms - r.line.ms for r in readings if r.shown]
    out += ["", "Readings: %d; the overlay was shown for %d of them (median %s ms, longest %s ms after the reading "
            "arrived)." % (len(readings), len(shown), show(median(shown)), show(max(shown) if shown else None)),
            "The click before it is not in the app log: the game writes the lines within about 1 s of the engine "
            "second of the click (M3, T3) and the app reads the log every 250 ms."]
    return out


def month_of(date: Tuple[int, int, int]) -> Tuple[int, int]:
    return date[0], date[1]


def game_day(date: Tuple[int, int, int]) -> Optional[int]:
    """The day number of a game date in the game's calendar, None for an impossible date."""
    year, month, day = date
    if not 1 <= month <= 12 or not 1 <= day <= MONTH_DAYS[month - 1]:
        return None
    return year * 365 + sum(MONTH_DAYS[:month - 1]) + day - 1


class IncomeCounts:
    def __init__(self) -> None:
        self.within = 0  # changes between two readings of one month
        self.across = 0  # changes across a month boundary
        self.unchanged = 0  # month boundaries without a change

    def add(self, boundary: bool, changed: bool) -> None:
        if boundary:
            if changed:
                self.across += 1
            else:
                self.unchanged += 1
        elif changed:
            self.within += 1

    def verdict(self) -> Tuple[str, str]:
        """(a short word for the series table, the sentence)."""
        if self.within:
            return "does not hold", "does not hold: the income changed within a month %d time(s)" % self.within
        if self.across + self.unchanged == 0:
            return "not tested", "not tested: no month boundary between two readings"
        if self.across == 0:
            return "not shown", ("not shown: the income did not change at a month boundary (nor within a month), so "
                                 "these readings cannot tell a monthly figure from a live one")
        return "holds", "holds for these readings: every change was across a month boundary"


def section_income(readings: List[Reading]) -> List[str]:
    out = ["## S3: income across month boundaries (criterion 10)", ""]
    usable = [r for r in readings if r.date is not None and r.income is not None]
    if len(usable) < 2:
        return out + ["Fewer than two readings with a date and an income figure."]
    rows = []
    overall = IncomeCounts()
    went_back = 0
    # A series: readings of one run, each at most SERIES_STEP_DAYS game days after the one before
    # (the steps of the monthly-tick test). (first reading, last reading, counts) per series.
    series: List[Tuple[Reading, Reading, IncomeCounts, int]] = [(usable[0], usable[0], IncomeCounts(), 1)]
    for before, after in zip(usable, usable[1:]):
        assert before.date is not None and after.date is not None
        changed = before.income != after.income
        apart = game_day(after.date) - game_day(before.date)  # type: ignore[operator]
        same_series = after.run is before.run and 0 <= apart <= SERIES_STEP_DAYS
        if apart < 0:
            boundary_text = "date went back"
            went_back += 1
        else:
            boundary = month_of(after.date) != month_of(before.date)
            boundary_text = "yes" if boundary else "no"
            overall.add(boundary, changed)
            if same_series:
                series[-1][2].add(boundary, changed)
        if same_series:
            first, _, counts, size = series[-1]
            series[-1] = (first, after, counts, size + 1)
        else:
            series.append((after, after, IncomeCounts(), 1))
        rows.append((before.date_text(), after.date_text(), apart, before.income, after.income, changed,
                     boundary_text, len(series) if same_series else "-"))
    out += table(("From", "To", "Game days apart", "Income before", "Income after", "Changed",
                  "Month boundary between", "Series"), rows)
    series_rows = [(number, first.date_text(), last.date_text(), size, counts.within, counts.across, counts.unchanged,
                    counts.verdict()[0])
                   for number, (first, last, counts, size) in enumerate(series, 1) if size >= 2]
    if series_rows:
        out += ["", "Series (readings of one run at most %d game days apart, as in the monthly-tick test):"
                % SERIES_STEP_DAYS, ""]
        out += table(("Series", "From", "To", "Readings", "Changes within a month", "Changes across a boundary",
                      "Boundaries without a change", "Hypothesis"), series_rows)
    out += ["", "All readings: changes within a month: %d; across a month boundary: %d; boundaries without a change: "
            "%d; date went back (a save loaded?): %d." % (overall.within, overall.across, overall.unchanged, went_back),
            "The hypothesis (the income changes only at the monthly tick): %s. A change within a month anywhere counts "
            "against it, unless another save or another ruler was played in between." % overall.verdict()[1]]
    return out


def section_asks(asks: List[Ask], refused: Dict[str, int], clock: Clock) -> List[str]:
    out = ["## Questions (ask_started to the end)", ""]
    if not asks:
        out.append("No question in this log.")
    else:
        rows = []
        for i, a in enumerate(asks, 1):
            end = a.end
            usage = end if end is not None and end.event == "ask_done" else None
            rows.append((i, a.run.number, clock.time(a.started.ms), a.gen, a.started.num("reading"),
                         a.started.num("snapshot_bytes"), a.started.num("snapshot_age_s"),
                         a.first_token.num("first_token_ms") if a.first_token else None,
                         end.num("total_ms") if end else None,
                         usage.num("input") if usage else None, usage.num("output") if usage else None,
                         usage.num("cache_write") if usage else None, usage.num("cache_read") if usage else None,
                         a.provider_end.word("stop_reason") if a.provider_end and a.provider_end.event == "chat_done"
                         else "-", a.result(),
                         a.net.num("status") if a.net else None, a.net.word("http") if a.net else "-",
                         a.net.num("connects") if a.net else None, a.net.word("conn") if a.net else "-"))
        out += table(("#", "Run", "Time", "Gen", "Reading", "Snapshot bytes", "Age (s)", "First token (ms)",
                      "Total (ms)", "In", "Out", "Cache write", "Cache read", "Stop reason", "End", "HTTP status",
                      "HTTP", "New connections", "Connection"), rows)
        done = [a for a in asks if a.end is not None and a.end.event == "ask_done"]
        firsts = [a.first_token.num("first_token_ms") for a in done if a.first_token and
                  a.first_token.num("first_token_ms") is not None]
        totals = [a.end.num("total_ms") for a in done if a.end and a.end.num("total_ms") is not None]
        out += ["", "Answered: %d of %d; median first token %s ms, median total %s ms." % (
            len(done), len(asks), show(median(firsts)), show(median(totals)))]  # type: ignore[arg-type]
    if refused:
        out += ["", "Refused (nothing was sent): " + ", ".join("%s %d" % (k, v) for k, v in sorted(refused.items()))
                + "."]
    return out


def section_cache(asks: List[Ask]) -> List[str]:
    out = ["## Cache (criterion 4: a second question within 5 minutes on the same reading)", ""]
    rows = []
    hits = 0
    for before, after in zip(asks, asks[1:]):
        if before.run is not after.run:
            continue
        apart = after.started.ms - before.started.ms
        reading_before, reading_after = before.started.num("reading"), after.started.num("reading")
        if reading_before is not None and reading_after is not None:
            same = reading_before == reading_after
        else:
            same = not any(line.event == "frame_complete" and before.started.ms <= line.ms <= after.started.ms
                           for line in before.run.lines)
        if apart > CACHE_WINDOW_MS or not same:
            continue
        usage = after.end if after.end is not None and after.end.event == "ask_done" else None
        read = usage.num("cache_read") if usage else None
        hit = read is not None and read > 0
        hits += hit
        rows.append((after.gen, before.gen, round(apart / 1000.0, 1), reading_after, read,
                     usage.num("cache_write") if usage else None,
                     "read from the cache" if hit else ("no cache read" if usage else "no answer")))
    if not rows:
        return out + ["No pair of questions within 5 minutes on the same reading."]
    out += table(("Question (gen)", "After (gen)", "Seconds apart", "Reading", "Cache read", "Cache write",
                  "Result"), rows)
    out += ["", "Pairs: %d; with cache_read > 0: %d." % (len(rows), hits)]
    return out


def section_stops(asks: List[Ask], clock: Clock) -> List[str]:
    out = ["## Stops (criterion 3: Stop, the request's end, the connection)", ""]
    stopped = [a for a in asks if a.end is not None and a.end.event == "ask_cancelled"]
    if not stopped:
        return out + ["No question was stopped."]
    rows = []
    for a in stopped:
        assert a.end is not None
        later = [b for b in asks if b.run is a.run and b.started.ms > a.end.ms and b.net is not None]
        following = later[0].net.num("connects") if later else None  # type: ignore[union-attr]
        gap = round((later[0].started.ms - a.end.ms) / 1000.0, 1) if later else None
        rows.append((a.gen, clock.time(a.end.ms), a.end.num("total_ms"), a.end.num("answer_bytes"),
                     a.net.ms - a.end.ms if a.net else None, a.net.word("outcome") if a.net else "-",
                     a.net.word("conn") if a.net else "-", a.net.word("http") if a.net else "-",
                     gap, following))
    out += table(("Question (gen)", "Stopped at", "Asked to stopped (ms)", "Answer bytes kept",
                  "Stop to request end (ms)", "Request end", "Connection", "HTTP", "Next request after (s)",
                  "Next request: new connections"), rows)
    out += ["", "ask_cancelled is written when Stop is pressed; net_done when the net thread has removed the "
            "transfer and freed it. \"Connection\" is the client's classification (net_done conn=), not an "
            "observation of the socket: \"closed\" is written for every transfer removed before its end on "
            "HTTP/1.x, whose connection libcurl 8.22 closes (lib/multi.c; the http tests check that the server "
            "sees it closed). The sign from this sitting is the next request's new connections: 1 means it did "
            "not reuse a kept connection. libcurl keeps an idle connection for at most 118 s, so that sign counts "
            "when the next request came sooner; compare with a question that followed a finished answer after a "
            "similar time (Questions table, \"New connections\" 0: it reused the connection)."]
    return out


def running_intervals(run: Run) -> Tuple[List[Tuple[int, int]], bool]:
    """CK3's running intervals within a run, from ck3_running; (whole run, False) if none was logged."""
    intervals: List[Tuple[int, int]] = []
    start: Optional[int] = None
    seen = False
    for line in run.lines:
        if line.event != "ck3_running":
            continue
        seen = True
        running = line.flag("running")
        if running and start is None:
            start = line.ms
        elif running is False and start is not None:
            intervals.append((start, line.ms))
            start = None
    if start is not None:
        intervals.append((start, run.end_ms))
    if not seen:
        return [(run.start_ms, run.end_ms)], False
    return intervals, True


class Window:
    """One hour (or the rest) of a stretch while CK3 ran."""

    def __init__(self, start: int, end: int) -> None:
        self.start = start
        self.end = end
        self.read = 0
        self.curia = 0
        self.readings = 0
        self.launch = False


class Stretch:
    """One stretch of a run while CK3 ran, from ck3_running=true to false (or the run's end), cut
    into hours. Its lines reach back LAUNCH_LEAD_MS before the start (not before the stretch before
    it ended), and a byte count up to BYTES_GRACE_MS after the end is still its own."""

    def __init__(self, number: int, start: int, end: int, earliest: int) -> None:
        self.number = number
        self.start = start
        self.end = end
        self.lead_start = max(start - LAUNCH_LEAD_MS, earliest)
        self.windows: List[Window] = []
        at = start
        while True:
            stop = min(at + HOUR_MS, end)
            self.windows.append(Window(at, stop))
            if stop >= end:
                break
            at = stop
        self.launched = False  # a log_truncated line: CK3 started during the run
        self.first_reading: Optional[int] = None
        self.steps: List[Tuple[int, int, int]] = []  # (ms, bytes, Curia's bytes) of each count

    def holds(self, ms: int) -> bool:
        return self.lead_start <= ms <= self.end

    def just_after(self, ms: int) -> bool:
        # The minute line after CK3 quit still carries the game's last bytes.
        return self.end < ms <= self.end + BYTES_GRACE_MS

    def window_for(self, ms: int) -> Window:
        return next((w for w in self.windows if ms <= w.end), self.windows[-1])

    def play(self) -> Tuple[int, int, int, bool]:
        """(milliseconds, bytes, Curia's bytes, whether any) of play: after a launch of CK3, from
        the count before the first reading on (by then the game had loaded: the Advisor button is
        in the loaded game), or from LAUNCH_FALLBACK_MS after the launch without a reading; without
        a launch, the whole stretch."""
        if not self.launched:
            return self.end - self.start, sum(s[1] for s in self.steps), sum(s[2] for s in self.steps), True
        boundary = self.first_reading if self.first_reading is not None else self.start + LAUNCH_FALLBACK_MS
        if boundary >= self.end:
            return 0, 0, 0, False
        before = [s[0] for s in self.steps if s[0] <= boundary]
        start = max([self.start] + before)
        after = [s for s in self.steps if s[0] > boundary]
        return max(0, self.end - start), sum(s[1] for s in after), sum(s[2] for s in after), True


def section_bytes(runs: List[Run], clock: Clock) -> List[str]:
    out = ["## S4: bytes per hour of debug.log while CK3 ran (criterion 8)", ""]
    rows = []
    stretch_rows = []
    total_ms = total = curia = readings_total = outside = 0
    # Play only: without CK3's launch and the loading of a save, which write far more than play.
    play_ms = play_total = play_curia = 0
    no_reading: List[str] = []  # launched, no reading: play from LAUNCH_FALLBACK_MS on
    no_play: List[str] = []  # launched and too short for any play
    unpolled: List[int] = []
    dropped: List[Tuple[int, int]] = []
    for run in runs:
        samples = []
        for line in run.lines:
            read, cur = (line.num("read"), line.num("curia")) if line.event == "log_bytes" else (None, None)
            if read is not None and cur is not None:
                samples.append((line.ms, read, cur))
        lost = max((line.num("mailbox_dropped") or 0 for line in run.lines if line.event == "log_bytes"), default=0)
        if lost:
            dropped.append((run.number, lost))
        if not samples:
            continue
        intervals, polled = running_intervals(run)
        if not polled:
            unpolled.append(run.number)
        stretches: List[Stretch] = []
        for start, end in intervals:
            earliest = stretches[-1].end + 1 if stretches else run.start_ms
            stretches.append(Stretch(len(stretches) + 1, start, end, earliest))
        for line in run.lines:
            if line.event not in ("log_truncated", "frame_complete"):
                continue
            stretch = next((s for s in stretches if s.holds(line.ms)), None)
            if stretch is None:
                continue
            window = stretch.window_for(line.ms)
            if line.event == "log_truncated":
                stretch.launched = window.launch = True
            else:
                window.readings += 1
                if stretch.first_reading is None:
                    stretch.first_reading = line.ms
        # The counts are cumulative from the start of the run; each step goes to the hour of the
        # line that reports it.
        before_read = before_curia = 0
        for ms, read, cur in samples:
            step_read, step_curia = max(0, read - before_read), max(0, cur - before_curia)
            before_read, before_curia = read, cur
            target = next((s for s in stretches if s.holds(ms)), None)
            if target is None:
                # Shortly after CK3 quit: the stretch that ended last before the line.
                ended = [s for s in stretches if s.just_after(ms)]
                target = max(ended, key=lambda s: s.end) if ended else None
            if target is None:
                outside += step_read
                continue
            target.steps.append((ms, step_read, step_curia))
            window = target.window_for(ms)
            window.read += step_read
            window.curia += step_curia
        for stretch in stretches:
            for number, w in enumerate(stretch.windows, 1):
                length = w.end - w.start
                rows.append((run.number, stretch.number, number, clock.time(w.start), clock.time(w.end),
                             round(length / 60000.0, 1), w.read, w.curia, w.read - w.curia, per_hour(w.read, length),
                             per_hour(w.read - w.curia, length), w.readings, w.launch))
                total_ms += length
                total += w.read
                curia += w.curia
                readings_total += w.readings
            length = stretch.end - stretch.start
            p_ms, p_bytes, p_curia, any_play = stretch.play()
            launch_bytes = sum(s[1] for s in stretch.steps) - p_bytes
            name = "run %d stretch %d" % (run.number, stretch.number)
            if any_play:
                play_ms += p_ms
                play_total += p_bytes
                play_curia += p_curia
                if stretch.launched and stretch.first_reading is None:
                    no_reading.append(name)
            else:
                no_play.append(name)
            stretch_rows.append((run.number, stretch.number, clock.time(stretch.start), clock.time(stretch.end),
                                 round(length / 60000.0, 1), stretch.launched,
                                 clock.time(stretch.first_reading) if stretch.first_reading is not None else "-",
                                 launch_bytes if stretch.launched else None,
                                 round(p_ms / 60000.0, 1) if any_play else None, p_bytes if any_play else None,
                                 p_curia if any_play else None, per_hour(p_bytes, p_ms) if any_play else None,
                                 per_hour(p_bytes - p_curia, p_ms) if any_play else None))
    if not rows:
        return out + ["No log_bytes line in this log (Curia did not read the game's log, or it is an older build)."]
    out += table(("Run", "Stretch", "Hour", "From", "To", "Minutes", "Bytes", "Curia's lines", "Without Curia's lines",
                  "Per hour", "Per hour without Curia's lines", "Readings", "CK3 launched in it"), rows)
    out += ["", "Stretches while CK3 ran (play: after a launch of CK3, from the byte count before its first "
            "reading on; the launch, the main menu and the loading of a save come before it):", ""]
    out += table(("Run", "Stretch", "From", "To", "Minutes", "CK3 launched in it", "First reading",
                  "Launch and loading bytes", "Play minutes", "Play bytes", "Play: Curia's lines", "Play per hour",
                  "Play per hour without Curia's lines"), stretch_rows)
    out += ["", "Overall while CK3 ran: %s minutes, %d bytes (%d of them Curia's lines, %d readings); per hour %s, "
            "without Curia's lines %s." % (round(total_ms / 60000.0, 1), total, curia, readings_total,
                                           show(per_hour(total, total_ms)), show(per_hour(total - curia, total_ms))),
            "Play only (without CK3's launch and the loading of a save): %s minutes, %d bytes (%d of them Curia's "
            "lines); per hour %s, without Curia's lines %s." % (
                round(play_ms / 60000.0, 1), play_total, play_curia, show(per_hour(play_total, play_ms)),
                show(per_hour(play_total - play_curia, play_ms))),
            "Per hour is printed for stretches of %d minutes or more. Game speed and paused time are in the owner's "
            "notes, not in the app log. A truncation or byte count up to %d s before CK3 was seen running belongs to "
            "that stretch (CK3 writes its log before the app's poll sees it)." % (
                MIN_WINDOW_MINUTES_FOR_RATE, LAUNCH_LEAD_MS // 1000)]
    if no_reading:
        out.append("Stretches in which CK3 was launched but no reading came (play counted from %d minutes after the "
                   "launch): %s." % (LAUNCH_FALLBACK_MS // 60000, ", ".join(no_reading)))
    if no_play:
        out.append("Stretches in which CK3 was launched that ended before any play was counted: %s." % ", ".join(
            no_play))
    if outside:
        out.append("Bytes read while CK3 was not running (not counted above): %d." % outside)
    if unpolled:
        out.append("Runs without a ck3_running line (the whole run is counted as CK3 running): %s." % ", ".join(
            str(n) for n in unpolled))
    if dropped:
        out.append("Messages the app's mailbox dropped (its limit; a reading may be among them), by run: %s." %
                   ", ".join("run %d: %d" % item for item in dropped))
    return out


def count_rows(lines: List[Line], keys: Sequence[str]) -> List[Tuple[Any, ...]]:
    counts: Dict[Tuple[str, ...], int] = {}
    for line in lines:
        key = tuple(line.word(k) for k in keys)
        counts[key] = counts.get(key, 0) + 1
    return [key + (n,) for key, n in sorted(counts.items())]


def section_overlay(runs: List[Run]) -> List[str]:
    lines = [line for run in runs for line in run.lines]
    out = ["## Overlay (criterion 7, Sitting A and B)", ""]
    shown = [l for l in lines if l.event == "overlay_shown"]
    if not shown and not any(l.event.startswith("overlay_") for l in lines):
        return out + ["No overlay line in this log (a headless run, or an older build)."]
    out += ["Shown (overlay_shown):", ""]
    out += table_or_none(("Reason", "Focus mode", "Panel key", "Curia active", "CK3 in front", "Count"),
                         count_rows(shown, ("reason", "focus", "panel_key", "app_active", "ck3_front")))
    hidden = [l for l in lines if l.event == "overlay_hidden"]
    out += ["", "Hidden (overlay_hidden): %d, of them with Curia active: %d." % (
        len(hidden), sum(1 for l in hidden if l.flag("app_active")))]
    out += ["", "Focus (overlay_focus):", ""]
    out += table_or_none(("Why", "Visible", "Panel key", "Curia active", "CK3 in front", "Count"),
                         count_rows([l for l in lines if l.event == "overlay_focus"],
                                    ("why", "visible", "panel_key", "app_active", "ck3_front")))
    out += ["", "Keys handed back (overlay_keys):", ""]
    out += table_or_none(("After", "Returned via", "To", "Accepted", "Count"),
                         count_rows([l for l in lines if l.event == "overlay_keys"],
                                    ("after", "returned", "to", "accepted")))
    placed = [l for l in lines if l.event == "overlay_placed"]
    out += ["", "Placed (overlay_placed):", ""]
    rows = {}
    for line in placed:
        key = (line.word("basis"), show(line.num("display")), show(line.num("displays")), line.word("game_window"))
        rows[key] = rows.get(key, 0) + 1
    out += table_or_none(("Basis", "Display", "Displays", "CK3 window found", "Count"),
                         [k + (n,) for k, n in sorted(rows.items())])
    out += ["", "Opened (overlay_opened, by source):", ""]
    out += table_or_none(("Source", "Count"),
                         count_rows([l for l in lines if l.event == "overlay_opened"], ("source",)))
    return out


def section_ck3(runs: List[Run], clock: Clock) -> List[str]:
    out = ["## CK3 and the notices", ""]
    rows = []
    for run in runs:
        for line in run.lines:
            if line.event == "ck3_running":
                rows.append((run.number, clock.time(line.ms), "ck3_running", line.word("running")))
            elif line.event == "notice":
                rows.append((run.number, clock.time(line.ms), "notice", line.word("name")))
            elif line.event in ("log_truncated", "frame_damaged", "frame_unsupported"):
                rows.append((run.number, clock.time(line.ms), line.event, "-"))
    if not rows:
        return out + ["No CK3 or notice line in this log."]
    return out + table(("Run", "Time", "Line", "Value"), rows)


def section_keys(hits: KeyHits, files: List[LogFile]) -> List[str]:
    def where(items: List[Tuple[int, int]]) -> str:
        shown_items = ["%d:%d" % (source + 1, number) for source, number in items[:50]]
        more = "" if len(items) <= 50 else " and %d more" % (len(items) - 50)
        return ", ".join(shown_items) + more
    out = ["## Key check (criterion 6)", ""]
    unread = [str(i + 1) for i, f in enumerate(files) if not f.readable]
    if unread:
        out.append("Not checked: file(s) %s could not be read (Files section)." % ", ".join(unread))
    if hits.key_shaped:
        out.append("FOUND: %d line(s) shaped like an API key (file:line): %s. Do not send the log; tell the "
                   "maintainer the count only." % (len(hits.key_shaped), where(hits.key_shaped)))
    else:
        out.append("No line shaped like an API key (sk-ant-..., or a long token of mixed letters and digits) in "
                   "the %d line(s) read." % sum(f.lines for f in files))
    if hits.redacted:
        out.append("Lines where the app replaced something key-like with [redacted] before writing (file:line): %s."
                   % where(hits.redacted))
    return out


def section_timeline(runs: List[Run], clock: Clock) -> List[str]:
    keep = ("frame_complete", "overlay_placed", "overlay_shown", "overlay_hidden", "overlay_focus", "overlay_keys",
            "overlay_opened", "ck3_running", "notice", "ask_started", "ask_done", "ask_cancelled", "ask_error",
            "net_done")
    words = {"overlay_placed": ("basis",), "overlay_shown": ("reason", "focus", "panel_key", "app_active", "ck3_front"),
             "overlay_hidden": ("app_active",), "overlay_focus": ("why", "visible", "panel_key", "app_active",
                                                                   "ck3_front"),
             "overlay_keys": ("after", "returned", "to", "accepted"), "overlay_opened": ("source",),
             "ck3_running": ("running",), "notice": ("name",), "ask_error": ("code",), "net_done": ("outcome", "conn")}
    numbers = {"frame_complete": ("reading", "snap"), "ask_started": ("gen",), "ask_done": ("gen",),
               "ask_cancelled": ("gen",), "ask_error": ("gen",)}
    rows = []
    for run in runs:
        for line in run.lines:
            if line.event not in keep:
                continue
            parts = ["%s=%s" % (k, line.word(k)) for k in words.get(line.event, ())]
            parts += ["%s=%s" % (k, show(line.num(k))) for k in numbers.get(line.event, ())]
            rows.append((run.number, clock.stamp(line.ms).strftime("%H:%M:%S.%f")[:-3], line.event, " ".join(parts)))
    return ["## Timeline", ""] + table(("Run", "Time", "Line", "Fields"), rows)


def analyse(paths: Sequence[str], utc: bool = False, timeline: bool = False) -> Tuple[str, int]:
    files, lines, hits = read_logs(paths)
    if not any(f.readable for f in files):
        return "", 2
    clock = Clock(utc)
    runs = split_runs(lines)
    asks = [ask for run in runs for ask in collect_asks(run)]
    refused: Dict[str, int] = {}
    for line in lines:
        if line.event == "ask_refused":
            refused[line.word("outcome")] = refused.get(line.word("outcome"), 0) + 1
    readings = collect_readings(runs)
    parts = ["# Curia app log: M4 sitting tables", "",
             "Numbers and the app's fixed words only. Times are %s." % clock.zone(), ""]
    for section in (section_files(files, lines, clock), section_runs(runs, clock),
                    section_readings(readings, clock), section_income(readings), section_asks(asks, refused, clock),
                    section_cache(asks), section_stops(asks, clock), section_bytes(runs, clock),
                    section_overlay(runs), section_ck3(runs, clock), section_keys(hits, files)):
        parts += section + [""]
    if timeline:
        parts += section_timeline(runs, clock) + [""]
    return "\n".join(parts), (1 if hits.key_shaped else 0)


def main(argv: Optional[Sequence[str]] = None) -> int:
    try:
        return run_main(argv)
    except Exception:
        # A fixed text only: an exception's message could quote a line of the log. Exit 2, so that
        # it never reads as 1 (a key-shaped line).
        print("analyze_applog: stopped by an internal error; nothing was printed (tell the maintainer)",
              file=sys.stderr)
        return 2


def run_main(argv: Optional[Sequence[str]]) -> int:
    parser = argparse.ArgumentParser(
        description="Tables of numbers from Curia's app log (curia.log) for the M4 game sittings.")
    parser.add_argument("paths", nargs="+",
                        help="app log files, or folders that hold curia.log (required: nothing is read unless named)")
    parser.add_argument("--utc", action="store_true", help="show times in UTC instead of the local time")
    parser.add_argument("--timeline", action="store_true",
                        help="also list the readings, overlay, CK3 and question lines in time order")
    try:
        args = parser.parse_args(argv)
    except SystemExit as stop:
        return 0 if stop.code == 0 else 2
    files = expand_paths(list(args.paths))
    if not any(os.path.isfile(p) for p in files):
        print("analyze_applog: no app log found (give the folder that holds curia.log, or the file)", file=sys.stderr)
        return 2
    text, code = analyse(files, utc=args.utc, timeline=args.timeline)
    if code == 2:
        print("analyze_applog: no app log could be read (permissions?)", file=sys.stderr)
        return 2
    sys.stdout.write(text)
    return code


if __name__ == "__main__":
    sys.exit(main())
