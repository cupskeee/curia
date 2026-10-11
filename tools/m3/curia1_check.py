#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reads CK3's debug.log (or a saved copy) and checks the CURIA1 records against docs/wire-format.md.

One-shot and read-only. For every export ("frame") it prints the realm as a table, a verdict (Complete or
Damaged, with the reason), and the values to compare with the game's own screen; at the end it prints a summary
(frame counts, text shortened, the printed form of every negative integer, every record that broke the grammar).

  python3 tools/m3/curia1_check.py "$LOGS/debug.log"
  python3 tools/m3/curia1_check.py ~/curia_m3_results/debug-prefreeze-a.log --last 1

Python 3.9+, standard library only. This file was written from sections 1 to 4 of the wire-format spec alone
(grammar, order of evaluation, ranges, oversize rule, frame state machine, checked counts); it shares no code
with the app, so a disagreement between the two is a finding. Only the record text (from the marker on) is
ever printed, never the engine's prefix, and lines without the marker are ignored.
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
from typing import Any, Dict, List, Optional, Tuple

MARKER = b"CURIA1|"
LINE_LIMIT = 4096  # bytes of a physical line kept (section 1)
RECORD_LIMIT = 1024  # a longer record is oversize (section 1)
MAX_FRAME_RECORDS = 512
MAX_FRAME_BYTES = 128 * 1024
MAXI = 2**31 - 1
MINI = -(2**31)

TYPE_RE = re.compile(rb"[A-Z][A-Z0-9_]*")
TOKEN_RE = re.compile(rb"[A-Za-z0-9._+-]{1,32}")
INT_RE = re.compile(rb"0|-?[1-9][0-9]{0,9}|-0")

TIER_WORDS = {1: "barony", 2: "county", 3: "duchy", 4: "kingdom", 5: "empire"}
FACTION_TYPES = {1: "independence", 2: "liberty", 3: "claimant", 4: "populist", 5: "peasant", 6: "nomadic", 7: "other"}
MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
          "November", "December"]
CLAIM_UI_WORD = {"strong": "pressed", "weak": "unpressed"}

# --------------------------------------------------------------------------------------------
# Grammar tables (section 4)
# --------------------------------------------------------------------------------------------


def _u(name):
    return (name, "int", 0, MAXI)


def _s(name):
    return (name, "sint", MINI, MAXI)


def _r(name, lo, hi):
    return (name, "int", lo, hi)


def _e(name, *values):
    return (name, "enum", values)


def _tok(name):
    return (name, "token")


class Form:
    """The fixed fields of one record form, the optional text field (always last) and the `none` form."""

    def __init__(self, fields, text=None, none_form=False):
        self.fields = fields
        self.text = text
        self.none_form = none_form


_CHAR_LINK = Form([_u("id")], "name", none_form=True)
FORMS: Dict[Tuple[str, Optional[str]], Form] = {
    ("LOAD", None): Form([_tok("mod_version")]),
    ("BEGIN", None): Form([_r("mode", 0, 1), _tok("mod_version"), _r("year", 0, 9999), _r("month", 1, 12),
                           _r("day", 1, 31), _u("ruler_id")], "ruler_name"),
    ("FIN", None): Form([_s("gold"), _s("income")]),
    ("HEIR", None): Form([_u("id"), _s("opinion"), _r("has_liege", 0, 1), _r("landed", 0, 1)], "name", True),
    ("VASSALS", None): Form([_u("total"), _u("listed")]),
    ("VASSAL", None): Form([_r("rank", 1, MAXI), _u("id"), _s("gold"), _s("opinion"), _u("strength")], "name"),
    ("WARS", None): Form([_u("total")]),
    ("WAR", "NAME"): Form([], "war_name"),
    ("WAR", "ATTACKER"): Form([_u("id")], "name"),
    ("WAR", "DEFENDER"): Form([_u("id")], "name"),
    ("CLAIMS", None): Form([_u("explicit"), _u("implicit"), _u("listed")]),
    ("CLAIM", None): Form([_r("n", 1, MAXI), _e("kind", b"strong", b"weak"), _r("tier", 1, 5)], "title"),
    ("FACTIONS", None): Form([_u("against"), _r("member", 0, 1), _u("listed")]),
    ("FACTION", "INFO"): Form([_e("role", b"against", b"member"), _r("type", 1, 7), _u("power"), _u("threshold"),
                               _u("members")]),
    ("FACTION", "LEADER"): _CHAR_LINK,
    ("FACTION", "CLAIMANT"): _CHAR_LINK,
    ("FACTION", "TARGET"): _CHAR_LINK,
    ("FACTION", "TITLE"): Form([_r("tier", 1, 5)], "title", True),
    ("END", None): Form([_u("count")]),
}
KNOWN_TYPES = {t for (t, _s_) in FORMS}
SUB_KINDS = {"WAR": ("NAME", "ATTACKER", "DEFENDER"),
             "FACTION": ("INFO", "LEADER", "CLAIMANT", "TITLE", "TARGET")}
SECTION_ORDER = ["FIN", "HEIR", "VASSALS", "WARS", "CLAIMS", "FACTIONS"]
ITEM_SECTION = {"VASSAL": "VASSALS", "WAR": "WARS", "CLAIM": "CLAIMS", "FACTION": "FACTIONS"}
WAR_GROUP = ["NAME", "ATTACKER", "DEFENDER"]
FACTION_GROUP = ["INFO", "LEADER", "CLAIMANT", "TITLE", "TARGET"]

# --------------------------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------------------------


def safe(raw: bytes, limit: int = 0) -> str:
    """Printable form of raw bytes: control bytes as \\xNN, invalid UTF-8 as U+FFFD, optionally cut."""
    text = raw.decode("utf-8", errors="replace")
    out = "".join(c if (c >= " " and c != "\x7f") else "\\x%02x" % ord(c) for c in text)
    if limit and len(out) > limit:
        out = out[:limit] + "..."
    return out


def hex_bytes(raw: bytes, limit: int = 12) -> str:
    shown = " ".join("%02x" % b for b in raw[:limit])
    return shown + (" ..." if len(raw) > limit else "")


def _removed_code_point(c: str) -> bool:
    o = ord(c)
    return (o < 0x20 or o == 0x7F or 0x80 <= o <= 0x9F or o in (0x2028, 0x2029) or 0x202A <= o <= 0x202E
            or 0x2066 <= o <= 0x2069)


def hygiene(raw: bytes) -> str:
    """Text hygiene of section 2, in the order the spec gives (display only; the checker never relies on it)."""
    s = raw.decode("utf-8", errors="replace")  # (1) repair invalid UTF-8
    s = s.replace("\x15!", "")  # (2)
    out: List[str] = []
    i, n = 0, len(s)
    while i < n:  # (3) link tags
        ch = s[i]
        if ch == "\x15":
            j = i + 1
            while j < n and s[j] != " ":
                j += 1
            word = s[i + 1:j]
            if word in ("L", "L;") or word.startswith("ONCLICK:") or word.startswith("TOOLTIP:"):
                i = j + 1 if j < n else j
                continue
        out.append(ch)
        i += 1
    s = "".join(c for c in out if not _removed_code_point(c))  # (4)
    return re.sub(" +", " ", s).strip(" ")  # (5)


def cut_utf8(raw: bytes, limit: int) -> bytes:
    """Cut to `limit` bytes, then drop up to 3 trailing bytes that are the start of an incomplete UTF-8 sequence."""
    cut = raw[:limit]
    for k in (1, 2, 3):
        if len(cut) < k:
            break
        lead = cut[-k]
        if 0xC2 <= lead <= 0xF4:
            need = 2 if lead < 0xE0 else 3 if lead < 0xF0 else 4
            tail = cut[len(cut) - k + 1:]
            if k < need and all(0x80 <= b <= 0xBF for b in tail):
                return cut[:-k]
    return cut


def parse_int(raw: bytes, lo: int, hi: int, signed: bool) -> Tuple[Optional[int], Optional[str]]:
    """Returns (value, None) or (None, why). `-0` is accepted only when `signed` and reads as 0."""
    if raw == b"":
        return None, "empty (an empty field is invalid outside the text field)"
    if not INT_RE.fullmatch(raw):
        if any(b < 0x20 or b >= 0x7F for b in raw):
            return None, "contains a control or non-ASCII byte (bytes %s)" % hex_bytes(raw)
        if re.fullmatch(rb"-?0[0-9]+", raw):
            return None, "leading zeros"
        if raw.startswith(b"+"):
            return None, "plus sign"
        if re.fullmatch(rb"-?[0-9]{11,}", raw):
            return None, "does not fit int32"
        return None, "not an int"
    if raw == b"-0":
        if not signed:
            return None, "-0 is valid only in a signed field"
        return 0, None
    value = int(raw)
    if value < MINI or value > MAXI:
        return None, "does not fit int32"
    if value < lo or value > hi:
        return None, "out of range (%d to %d)" % (lo, hi)
    return value, None


# --------------------------------------------------------------------------------------------
# Layer 1: physical lines
# --------------------------------------------------------------------------------------------


def split_lines(data: bytes):
    """Returns ([(line_no, head, overlong)], tail): every terminated line, and the unterminated rest.

    The head is the physical line without the newline, cut to LINE_LIMIT bytes; one \\r directly before the
    newline is removed (not for an overlong line, whose newline lies in the dropped part)."""
    parts = data.split(b"\n")
    tail = parts.pop()
    lines = []
    for number, raw in enumerate(parts, 1):
        if len(raw) > LINE_LIMIT:
            lines.append((number, raw[:LINE_LIMIT], True))
        else:
            lines.append((number, raw[:-1] if raw.endswith(b"\r") else raw, False))
    return lines, tail


# --------------------------------------------------------------------------------------------
# Layer 2: one line -> not a Curia line | invalid record | record
# --------------------------------------------------------------------------------------------


class Parsed:
    """kind: 'none' (not a Curia line), 'invalid' or 'record'."""

    def __init__(self, kind, line_no=0, record=b""):
        self.kind = kind
        self.line_no = line_no
        self.record = record  # the text from the marker to the end of the line head (maybe cut)
        # invalid:
        self.snap: Optional[int] = None  # None when the snap did not parse
        self.type: Optional[str] = None  # None when the type did not match the pattern
        self.reason = ""
        self.body: Optional[bytes] = None
        # record:
        self.sub: Optional[str] = None
        self.n: Optional[int] = None
        self.values: Dict[str, Any] = {}
        self.text: Optional[bytes] = None
        self.none = False
        self.shortened = False
        self.unknown = False
        self.length = 0
        self.negatives: List[Tuple[str, str]] = []  # (TYPE.field, printed form)


def _invalid(line_no, record, reason, snap=None, rtype=None, body=None):
    p = Parsed("invalid", line_no, record)
    p.reason, p.snap, p.type, p.body = reason, snap, rtype, body
    return p


def _split_header(rec: bytes):
    rest = rec[len(MARKER):]
    parts = rest.split(b"|", 2)
    snap_raw = parts[0]
    type_raw = parts[1] if len(parts) > 1 else None
    body = parts[2] if len(parts) > 2 else None
    return snap_raw, type_raw, body


def _parse_fields(label: str, form: Form, body: Optional[bytes], allow_text: bool):
    """Checks `body` (the bytes after `TYPE|`, None if there was no `|`) against `form`.

    Returns (values, text, none, negatives, error)."""
    values: Dict[str, Any] = {}
    negatives: List[Tuple[str, str]] = []
    if form.none_form and body is not None:
        first = body.split(b"|", 1)[0]
        if first == b"none":
            if body == b"none":
                return {}, None, True, negatives, None
            return None, None, False, negatives, "%s: bytes after `none` (the none form has nothing after the word)" % label
    k = len(form.fields)
    if form.text is not None:
        parts = [] if body is None else body.split(b"|", k)
        if len(parts) < k:
            return None, None, False, negatives, "%s: too few fields (expected %d fixed fields and a text field, got %d)" % (
                label, k, len(parts))
        if len(parts) == k:
            return None, None, False, negatives, "%s: text delimiter missing (the `|` before the text field is mandatory, " \
                "even when the text is empty)" % label
        text = parts[k]
        parts = parts[:k]
    else:
        parts = [] if body is None else body.split(b"|")
        text = None
        if len(parts) < k:
            return None, None, False, negatives, "%s: too few fields (expected %d, got %d)" % (label, k, len(parts))
        if len(parts) > k:
            return None, None, False, negatives, "%s: too many fields (expected %d, got %d; this record has no text field)" % (
                label, k, len(parts))
    for spec, raw in zip(form.fields, parts):
        name, kind = spec[0], spec[1]
        where = "%s field %d `%s`" % (label, len(values) + 1, name)
        if kind in ("int", "sint"):
            value, why = parse_int(raw, spec[2], spec[3], kind == "sint")
            if why:
                return None, None, False, negatives, "%s: %s (got %s)" % (where, why, safe(raw, 40) or "nothing")
            values[name] = value
            if raw.startswith(b"-"):
                negatives.append(("%s.%s" % (label, name), raw.decode("ascii")))
        elif kind == "enum":
            if raw not in spec[2]:
                return None, None, False, negatives, "%s: not one of %s (got %s; the words are case-sensitive)" % (
                    where, "/".join(v.decode() for v in spec[2]), safe(raw, 40) or "nothing")
            values[name] = raw.decode()
        else:  # token
            if not TOKEN_RE.fullmatch(raw):
                return None, None, False, negatives, "%s: not a token of 1 to 32 characters from A-Z a-z 0-9 . _ + - (got %s)" % (
                    where, safe(raw, 40) or "nothing")
            values[name] = raw.decode()
    return values, text, False, negatives, None


def parse_line(head: bytes, overlong: bool = False, line_no: int = 0) -> Parsed:
    """Section 2 order of evaluation for one physical line (head, \\r already removed)."""
    idx = head.find(MARKER)
    if idx <= 0:  # no marker, or the marker at offset 0 (the engine always writes a prefix)
        return Parsed("none", line_no)
    full = head[idx:]
    snap_raw, type_raw, body = _split_header(full)
    # 2. the header
    snap, why = parse_int(snap_raw, 0, MAXI, False)
    rtype = type_raw.decode("ascii") if type_raw is not None and TYPE_RE.fullmatch(type_raw) else None
    if why or rtype is None:
        reason = ("header: snap %s (got %s)" % (why, safe(snap_raw, 40) or "nothing")) if why else \
            "header: the type is missing or does not match [A-Z][A-Z0-9_]* (got %s)" % (
                safe(type_raw, 40) if type_raw is not None else "nothing")
        return _invalid(line_no, full, reason, snap, rtype, body)
    # 3. snap 0 only on LOAD
    if rtype == "LOAD" and snap != 0:
        return _invalid(line_no, full, "LOAD must have snap 0 (got %d)" % snap, snap, rtype, body)
    if snap == 0 and rtype != "LOAD":
        return _invalid(line_no, full, "snap 0 is valid only on LOAD (got %s)" % rtype, snap, rtype, body)
    # 4. unknown types (the rest of the line is not inspected)
    sub = None
    n = None
    form_body = body
    if rtype in SUB_KINDS:
        head_parts = (body if body is not None else b"").split(b"|", 2)
        n_raw = head_parts[0] if body is not None else None
        n, why = parse_int(n_raw, 1, MAXI, False) if n_raw is not None else (None, "missing")
        if why:
            return _invalid(line_no, full, "%s: `n` must be an int of 1 or more (%s)" % (rtype, why), snap, rtype, body)
        if len(head_parts) < 2:
            return _invalid(line_no, full, "%s without a sub-kind" % rtype, snap, rtype, body)
        if not TYPE_RE.fullmatch(head_parts[1]):
            return _invalid(line_no, full, "%s: the sub-kind is empty or does not match [A-Z][A-Z0-9_]* (got %s)" % (
                rtype, safe(head_parts[1], 40) or "nothing"), snap, rtype, body)
        sub = head_parts[1].decode("ascii")
        form_body = head_parts[2] if len(head_parts) > 2 else None
    known = rtype in KNOWN_TYPES and (rtype not in SUB_KINDS or sub in SUB_KINDS[rtype])
    if not known:
        p = Parsed("record", line_no, full)
        p.snap, p.type, p.sub, p.n, p.unknown, p.length = snap, rtype, sub, n, True, len(full)
        return p
    # 5. a known type: oversize rule, then the field grammar
    oversize = overlong or len(full) > RECORD_LIMIT
    rec = cut_utf8(full, RECORD_LIMIT) if len(full) > RECORD_LIMIT else full
    if oversize and rec is not full:
        _, _, body = _split_header(rec)
        if rtype in SUB_KINDS:
            head_parts = (body if body is not None else b"").split(b"|", 2)
            form_body = head_parts[2] if len(head_parts) > 2 else None
        else:
            form_body = body
    label = rtype if sub is None else "%s %s" % (rtype, sub)
    form = FORMS[(rtype, sub)]
    values, text, none, negatives, error = _parse_fields(label, form, form_body, True)
    if error:
        if oversize:
            error += " [oversize record: the fixed fields and the `|` before the text must lie within the first %d bytes]" % RECORD_LIMIT
        return _invalid(line_no, rec, error, snap, rtype, body)
    if oversize and text is None:
        return _invalid(line_no, rec, "%s: oversize record (%s) without a text field" % (
            label, "from an overlong line" if overlong else "%d bytes, limit %d" % (len(full), RECORD_LIMIT)), snap, rtype, body)
    p = Parsed("record", line_no, rec)
    p.snap, p.type, p.sub, p.n = snap, rtype, sub, n
    p.values, p.text, p.none, p.negatives = values, text, none, negatives
    p.shortened = oversize
    p.length = len(rec)
    return p


# --------------------------------------------------------------------------------------------
# Layer 3: the frame assembler (section 3)
# --------------------------------------------------------------------------------------------


class Frame:
    def __init__(self, begin: Parsed):
        self.snap = begin.snap
        self.begin = begin
        self.recs: List[Parsed] = []  # every other frame record after BEGIN, END excluded
        self.end: Optional[Parsed] = None
        self.count = 1
        self.nbytes = begin.length
        self.shortened = bool(begin.shortened)
        self.status = "open"  # open | complete | damaged
        self.reason = ""
        self.detail = ""
        self.ignored_after_damage = 0
        self.first_line = begin.line_no
        self.last_line = begin.line_no


class Assembler:
    IDLE, OPEN, DAMAGED = "idle", "open", "damaged"

    def __init__(self):
        self.state = self.IDLE
        self.frame: Optional[Frame] = None
        self.frames: List[Frame] = []
        self.events: List[Tuple[str, Optional[int], str]] = []  # (kind, snap, reason)
        self.unsupported: List[Tuple[int, str]] = []  # (line, report text)
        self.invalid: List[Parsed] = []
        self.loads: List[Tuple[int, str]] = []
        self.unknown_counts: collections.Counter = collections.Counter()
        self.negatives: collections.Counter = collections.Counter()
        self.curia_lines = 0
        self.snap0_ignored = 0
        self.hygiene_changed = 0
        self.hygiene_by_type: collections.Counter = collections.Counter()

    # -- events
    def _damage(self, frame: Frame, reason: str, detail: str):
        frame.status, frame.reason, frame.detail = "damaged", reason, detail
        self.events.append(("damaged", frame.snap, reason))
        self.state = self.DAMAGED

    def _report_unsupported(self, p: Parsed):
        fields = (p.body if p.body is not None else b"").split(b"|")[:3]
        raw = b"|".join(fields)[:64]
        shown = "".join("?" if (b < 0x20 or b == 0x7F) else chr(b) if b < 0x7F else "�" for b in raw)
        text = "unsupported mod format (snap %d, line %d): first fields after BEGIN: %s" % (p.snap, p.line_no, shown)
        self.unsupported.append((p.line_no, text))
        self.events.append(("unsupported", p.snap, "unsupported mod format"))

    # -- input
    def feed(self, p: Parsed):
        if p.kind == "none":
            return
        self.curia_lines += 1
        if p.kind == "invalid":
            self._invalid(p)
        else:
            self._record(p)

    def _invalid(self, p: Parsed):
        self.invalid.append(p)
        if p.snap == 0:  # any record with snap 0 never touches a frame
            return
        if p.snap is not None and p.snap >= 1 and p.type == "BEGIN":
            if self.state == self.OPEN and self.frame is not None:
                self._damage(self.frame, "incomplete", "line %d: an unreadable BEGIN arrived before END" % p.line_no)
            self._report_unsupported(p)
            self.state, self.frame = self.IDLE, None
            return
        if self.state == self.OPEN and self.frame is not None:
            self._damage(self.frame, "invalid record", "line %d: %s" % (p.line_no, p.reason))

    def _record(self, p: Parsed):
        for field, printed in p.negatives:
            self.negatives[(field, printed)] += 1
        if p.unknown:
            self.unknown_counts[p.type if p.sub is None else "%s.%s" % (p.type, p.sub)] += 1
        if p.text is not None and hygiene(p.text) != p.text.decode("utf-8", errors="replace"):
            self.hygiene_changed += 1
            self.hygiene_by_type[p.type if p.sub is None else "%s %s" % (p.type, p.sub)] += 1
        if p.snap == 0:
            self.snap0_ignored += 1
            if p.type == "LOAD":
                self.loads.append((p.line_no, p.values["mod_version"]))
            return
        if p.type == "BEGIN" and not p.unknown:
            if self.state == self.OPEN and self.frame is not None:
                self._damage(self.frame, "incomplete",
                             "a new BEGIN (snap %d, line %d) arrived before END" % (p.snap, p.line_no))
            self.frame = Frame(p)
            self.frames.append(self.frame)
            self.state = self.OPEN
            return
        if self.state != self.OPEN or self.frame is None:
            if self.state == self.DAMAGED and self.frame is not None:
                self.frame.ignored_after_damage += 1
                if p.type == "END":  # the damaged frame ends here, no second event
                    self.state, self.frame = self.IDLE, None
            return
        f = self.frame
        f.last_line = p.line_no
        if f.count + 1 > MAX_FRAME_RECORDS or f.nbytes + p.length > MAX_FRAME_BYTES:
            self._damage(f, "too large", "line %d: the frame passed %d records or %d bytes" % (
                p.line_no, MAX_FRAME_RECORDS, MAX_FRAME_BYTES))
            return
        f.count += 1
        f.nbytes += p.length
        if p.type == "END" and not p.unknown:
            f.end = p
            self._close(f, p)
            return
        f.recs.append(p)
        if p.snap != f.snap:
            self._damage(f, "snap mismatch", "line %d: a %s record has snap %d, the frame has snap %d" % (
                p.line_no, p.type, p.snap, f.snap))
            return
        if p.shortened:
            f.shortened = True

    def _close(self, f: Frame, end: Parsed):
        found: List[Tuple[str, str]] = []
        if end.snap != f.snap:
            found.append(("snap mismatch", "END has snap %d, the frame has snap %d" % (end.snap, f.snap)))
        else:
            if end.values["count"] != f.count:
                found.append(("count mismatch", "END says %d records, the frame has %d (BEGIN, END and unknown "
                              "types included)" % (end.values["count"], f.count)))
            found.extend(walk_sections(f)[1])
        for reason in ("snap mismatch", "count mismatch", "order"):
            for kind, detail in found:
                if kind == reason:
                    self._damage(f, reason, detail)
                    self.state, self.frame = self.IDLE, None
                    return
        f.status = "complete"
        self.events.append(("complete", f.snap, "text shortened" if f.shortened else ""))
        self.state, self.frame = self.IDLE, None


# --------------------------------------------------------------------------------------------
# Section rules and checked counts (section 3)
# --------------------------------------------------------------------------------------------


def walk_sections(frame: Frame):
    """Groups the stored records into sections and returns (sections, violations).

    violations: list of (kind, detail), kind 'count mismatch' or 'order'. A section is {'head': rec, 'items': [...]}.
    Where the spec does not say which of the two a violation is, a number that disagrees with a count is a
    'count mismatch' and a wrong position, numbering or grouping is 'order'."""
    sections: Dict[str, Dict[str, Any]] = {}
    bad: List[Tuple[str, str]] = []
    current: Optional[str] = None
    last_index = -1
    for r in frame.recs:
        if r.unknown:
            continue
        if r.type in SECTION_ORDER:
            if r.type in sections:
                bad.append(("order", "line %d: %s appears twice" % (r.line_no, r.type)))
                continue
            index = SECTION_ORDER.index(r.type)
            if index < last_index:
                bad.append(("order", "line %d: %s is out of order (sections go %s)" % (
                    r.line_no, r.type, ", ".join(SECTION_ORDER))))
            last_index = max(last_index, index)
            sections[r.type] = {"head": r, "items": []}
            current = r.type
        elif r.type in ITEM_SECTION:
            if current != ITEM_SECTION[r.type] or ITEM_SECTION[r.type] not in sections:
                bad.append(("order", "line %d: a %s record outside its section %s" % (
                    r.line_no, r.type, ITEM_SECTION[r.type])))
                continue
            sections[current]["items"].append(r)
    v = sections.get("VASSALS")
    if v:
        items, h = v["items"], v["head"].values
        if h["listed"] != len(items):
            bad.append(("count mismatch", "VASSALS says listed %d, %d VASSAL records follow" % (h["listed"], len(items))))
        if [i.values["rank"] for i in items] != list(range(1, len(items) + 1)):
            bad.append(("order", "VASSAL rank does not run 1..%d in order" % len(items)))
        if h["listed"] > h["total"]:
            bad.append(("count mismatch", "VASSALS listed %d is more than total %d" % (h["listed"], h["total"])))
    w = sections.get("WARS")
    if w:
        groups, problem = _groups(w["items"], WAR_GROUP, "WAR")
        if problem:
            bad.append(("order", problem))
        if len(groups) != w["head"].values["total"] and not problem:
            bad.append(("count mismatch", "WARS says total %d, %d WAR groups follow" % (
                w["head"].values["total"], len(groups))))
        w["groups"] = groups
    c = sections.get("CLAIMS")
    if c:
        items, h = c["items"], c["head"].values
        if h["listed"] != len(items):
            bad.append(("count mismatch", "CLAIMS says listed %d, %d CLAIM records follow" % (h["listed"], len(items))))
        if [i.values["n"] for i in items] != list(range(1, len(items) + 1)):
            bad.append(("order", "CLAIM n does not run 1..%d in order" % len(items)))
        if h["listed"] > h["explicit"]:
            bad.append(("count mismatch", "CLAIMS listed %d is more than explicit %d" % (h["listed"], h["explicit"])))
    fa = sections.get("FACTIONS")
    if fa:
        groups, problem = _groups(fa["items"], FACTION_GROUP, "FACTION")
        h = fa["head"].values
        if problem:
            bad.append(("order", problem))
        else:
            if len(groups) != h["listed"]:
                bad.append(("count mismatch", "FACTIONS says listed %d, %d FACTION groups follow" % (h["listed"], len(groups))))
            members = sum(1 for g in groups if g[0].values["role"] == "member")
            against = sum(1 for g in groups if g[0].values["role"] == "against")
            if members != h["member"]:
                bad.append(("count mismatch", "FACTIONS says member %d, %d groups have role member" % (h["member"], members)))
            if against > h["against"]:
                bad.append(("count mismatch", "FACTIONS says against %d, but %d groups have role against" % (h["against"], against)))
        fa["groups"] = groups
    return sections, bad


def _groups(items, pattern, label):
    """Splits adjacent items into groups of len(pattern) records with sub-kinds in order and one shared n."""
    groups = []
    size = len(pattern)
    for g in range(0, len(items), size):
        chunk = items[g:g + size]
        if [r.sub for r in chunk] != pattern:
            return groups, "%s group %d (line %d) is not %s, adjacent and in that order" % (
                label, g // size + 1, chunk[0].line_no, ", ".join(pattern))
        if len({r.n for r in chunk}) != 1:
            return groups, "%s group %d (line %d): its records do not all carry the same n" % (label, g // size + 1, chunk[0].line_no)
        if chunk[0].n != g // size + 1:
            return groups, "%s group %d (line %d) has n %d, expected %d" % (label, g // size + 1, chunk[0].line_no,
                                                                          chunk[0].n, g // size + 1)
        groups.append(chunk)
    return groups, ""


# --------------------------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------------------------


def tenths(v: int) -> str:
    sign = "-" if v < 0 else ""
    return "%s%d.%d" % (sign, abs(v) // 10, abs(v) % 10)


def name_text(p: Parsed) -> str:
    if p.text is None:
        return ""
    clean = hygiene(p.text)
    return clean if clean else "(empty)"


def _table(headers, rows, indent="  "):
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(str(cell)))
    out = [indent + "  ".join(h.ljust(widths[i]) for i, h in enumerate(headers)).rstrip()]
    for row in rows:
        out.append(indent + "  ".join(str(c).ljust(widths[i]) for i, c in enumerate(row)).rstrip())
    return out


def frame_lines(frame: Frame) -> List[str]:
    b = frame.begin.values
    sections, _ = walk_sections(frame)
    out: List[str] = []
    mode = "Advisor click" if b["mode"] == 0 else "probe click"
    out.append("  Ruler        %s (id %d)" % (name_text(frame.begin) or "(empty)", b["ruler_id"]))
    out.append("  Date         %04d-%02d-%02d  (year %d, month %d = %s, day %d)" % (
        b["year"], b["month"], b["day"], b["year"], b["month"], MONTHS[b["month"] - 1], b["day"]))
    out.append("  Export       mod_version %s, mode %d (%s)" % (b["mod_version"], b["mode"], mode))
    fin = sections.get("FIN")
    if fin:
        g, i = fin["head"].values["gold"], fin["head"].values["income"]
        out.append("  Gold         %d" % g)
        out.append("  Income       %d tenths = %s gold per month (net: income minus expenses)" % (i, tenths(i)))
    else:
        out.append("  Gold/income  not exported")
    heir = sections.get("HEIR")
    if heir:
        h = heir["head"]
        if h.none:
            out.append("  Heir         none")
        else:
            v = h.values
            out.append("  Heir         %s (id %d), opinion of the heir's liege %d, has liege %s, landed %s" % (
                name_text(h), v["id"], v["opinion"], "yes" if v["has_liege"] else "no", "yes" if v["landed"] else "no"))
    else:
        out.append("  Heir         not exported")
    vas = sections.get("VASSALS")
    if vas:
        v = vas["head"].values
        out.append("  Vassals      %d in total, %d listed" % (v["total"], v["listed"]))
        out.extend(_table(["rank", "id", "gold", "opinion", "strength", "name"],
                          [(i.values["rank"], i.values["id"], i.values["gold"], i.values["opinion"],
                            i.values["strength"], name_text(i)) for i in vas["items"]], "    "))
    else:
        out.append("  Vassals      not exported")
    war = sections.get("WARS")
    if war:
        out.append("  Wars         %d in total" % war["head"].values["total"])
        for g in war.get("groups", []):
            nm, at, de = g
            if b["ruler_id"] == at.values["id"]:
                side = "the ruler is the ATTACKER"
            elif b["ruler_id"] == de.values["id"]:
                side = "the ruler is the DEFENDER"
            else:
                side = "the ruler is neither id: an ally (the ids do not tell which side)"
            out.append("    war %d: %s" % (nm.n, name_text(nm) or "(empty)"))
            out.append("      attacker %s (id %d); defender %s (id %d); %s" % (
                name_text(at), at.values["id"], name_text(de), de.values["id"], side))
    else:
        out.append("  Wars         not exported")
    cl = sections.get("CLAIMS")
    if cl:
        v = cl["head"].values
        out.append("  Claims       explicit %d, implicit %d, listed %d" % (v["explicit"], v["implicit"], v["listed"]))
        out.extend(_table(["n", "state in the UI", "(enum)", "tier", "title"],
                          [(i.values["n"], CLAIM_UI_WORD[i.values["kind"]], i.values["kind"],
                            "%d %s" % (i.values["tier"], TIER_WORDS[i.values["tier"]]), name_text(i))
                           for i in cl["items"]], "    "))
    else:
        out.append("  Claims       not exported")
    fa = sections.get("FACTIONS")
    if fa:
        v = fa["head"].values
        out.append("  Factions     %d against the ruler, ruler is a member of one: %s, %d listed" % (
            v["against"], "yes" if v["member"] else "no", v["listed"]))
        for g in fa.get("groups", []):
            info = g[0].values
            out.append("    faction %d: role %s, type %d %s, power %d, threshold %d, members %d" % (
                g[0].n, info["role"], info["type"], FACTION_TYPES[info["type"]], info["power"], info["threshold"],
                info["members"]))
            for r in g[1:]:
                label = r.sub.lower()
                if r.none:
                    shown = "none"
                elif r.sub == "TITLE":
                    shown = "%d %s: %s" % (r.values["tier"], TIER_WORDS[r.values["tier"]], name_text(r))
                else:
                    shown = "%s (id %d)" % (name_text(r), r.values["id"])
                note = ""
                if r.sub in ("CLAIMANT", "TITLE") and not r.none and info["type"] != 3:
                    note = "   [type is not claimant: the app ignores this]"
                out.append("      %-9s %s%s" % (label + ":", shown, note))
    else:
        out.append("  Factions     not exported")
    return out


def compare_lines(frame: Frame) -> List[str]:
    """What to compare with the game's own screen (the must-test cases of wire-format.md section 9)."""
    b = frame.begin.values
    sections, _ = walk_sections(frame)
    out: List[str] = []
    out.append("  [H] Date: the top bar must show day %d, month %s (month %d), year %d. A month one off means the "
               "month base is wrong." % (b["day"], MONTHS[b["month"] - 1], b["month"], b["year"]))
    fin = sections.get("FIN")
    if fin:
        g, i = fin["head"].values["gold"], fin["head"].values["income"]
        out.append("  [E] Gold: the top bar must show %d gold (whole gold)." % g)
        out.append("  [E] Income: the log figure is the NET monthly balance. The top bar must show %s%s a month and the "
                   "tooltip's total line must show the same (%d tenths in the log); the tooltip's income line alone "
                   "is the income before expenses. Write the total next to it." % ("+" if i > 0 else "", tenths(i), i))
    vas = sections.get("VASSALS")
    if vas:
        v = vas["head"].values
        out.append("  [E] Vassals window: %d vassals in total. Rank 1 to %d by strength: compare each vassal's gold, "
                   "opinion and strength. If the window does not show gold or strength, that field fails D15."
                   % (v["total"], v["listed"]))
    if sections.get("HEIR") and not sections["HEIR"]["head"].none:
        h = sections["HEIR"]["head"].values
        out.append("  Heir: the character window must show the heir named above, with opinion %d of the heir's liege." % h["opinion"])
    war = sections.get("WARS")
    if war:
        out.append("  [D] Wars: the game must list %d wars for the ruler (allied wars included). Compare each war's "
                   "name and which side the ruler is on." % war["head"].values["total"])
    cl = sections.get("CLAIMS")
    if cl:
        v = cl["head"].values
        out.append("  Claims: the ruler's character window should list the %d explicit claim(s), each pressed or "
                   "unpressed as shown above (log strong = pressed, weak = unpressed); the %d implicit claim(s) are "
                   "only counted, the window may not list them." % (v["explicit"], v["implicit"]))
    fa = sections.get("FACTIONS")
    if fa:
        v = fa["head"].values
        out.append("  [A/B] Factions window: %d faction(s) against the ruler, and the ruler %s a faction." % (
            v["against"], "has joined" if v["member"] else "has not joined"))
        for g in fa.get("groups", []):
            info = g[0].values
            out.append("    faction %d: type %s, power %d%%, members %d, leader %s, target %s. Does the window show a "
                       "threshold (%d)? If not, threshold fails D15." % (
                           g[0].n, FACTION_TYPES[info["type"]], info["power"], info["members"],
                           "none" if g[1].none else name_text(g[1]), "none" if g[4].none else name_text(g[4]),
                           info["threshold"]))
            if info["type"] == 3:
                out.append("      claimant: %s; title wanted: %s. The window should name both." % (
                    "none" if g[2].none else name_text(g[2]), "none" if g[3].none else name_text(g[3])))
    negs = [p for p in [frame.begin] + frame.recs if p.negatives]
    if negs:
        out.append("  [E] Negative integers in this frame: %s." % ", ".join(
            "%s %s" % (field, printed) for p in negs for field, printed in p.negatives))
    return out


def verdict_line(frame: Frame) -> str:
    if frame.status == "complete":
        return "Complete" + ("; text shortened (a name was longer than 1,024 bytes)" if frame.shortened else "")
    if frame.status == "damaged":
        return "Damaged: %s (%s)" % (frame.reason, frame.detail)
    return "Not finished: no END until the end of the file (still being written, or lost)"


def report(asm: Assembler, lines_total: int, overlong: int, tail: bytes, last: Optional[int], out) -> int:
    frames = asm.frames
    shown = frames if last is None else frames[len(frames) - min(last, len(frames)):]
    for frame in shown:
        out.write("=" * 78 + "\n")
        out.write("Frame snap %d, lines %d to %d, %d records\n" % (frame.snap, frame.first_line, frame.last_line,
                                                                frame.count))
        out.write("VERDICT: %s\n" % verdict_line(frame))
        if frame.status != "complete":
            out.write("(The app throws this frame away. Below is what arrived before the problem.)\n")
        out.write("\n".join(frame_lines(frame)) + "\n")
        out.write("COMPARE WITH THE GAME UI\n" + "\n".join(compare_lines(frame)) + "\n")
    if last is not None and len(shown) < len(frames):
        out.write("(%d earlier frame(s) not shown; the summary below counts all of them)\n" % (len(frames) - len(shown)))
    complete = sum(1 for f in frames if f.status == "complete")
    damaged = [f for f in frames if f.status == "damaged"]
    unfinished = sum(1 for f in frames if f.status == "open")
    out.write("=" * 78 + "\nSUMMARY\n")
    out.write("  physical lines read: %d; Curia lines: %d; overlong lines (over %d bytes): %d\n" % (
        lines_total, asm.curia_lines, LINE_LIMIT, overlong))
    if tail:
        out.write("  unterminated last line ignored: %d bytes (the game may still be writing it)\n" % len(tail))
    out.write("  frames: %d (Complete %d, Damaged %d, not finished %d)\n" % (len(frames), complete, len(damaged), unfinished))
    for f in damaged:
        out.write("    damaged: snap %d, lines %d to %d: %s (%s)\n" % (f.snap, f.first_line, f.last_line, f.reason, f.detail))
    out.write("  frames with text shortened: %d\n" % sum(1 for f in frames if f.shortened))
    out.write("  text fields changed by the hygiene rules (link markup, control bytes): %d%s\n" % (
        asm.hygiene_changed, "" if not asm.hygiene_by_type else " (" + ", ".join(
            "%s x%d" % (k, v) for k, v in sorted(asm.hygiene_by_type.items())) + ")"))
    out.write("  LOAD records (snap 0): %s\n" % (
        ", ".join("line %d version %s" % (ln, tok) for ln, tok in asm.loads) if asm.loads else "none"))
    if asm.unknown_counts:
        out.write("  unknown record types (ignored, counted): %s\n" % ", ".join(
            "%s x%d" % (k, v) for k, v in sorted(asm.unknown_counts.items())))
    if asm.negatives:
        out.write("  negative integers seen (field, printed form, times):\n")
        for (field, printed), times in sorted(asm.negatives.items()):
            out.write("    %s  %s  sign byte 0x%02x (ASCII hyphen-minus)  x%d\n" % (field, printed, printed.encode()[0], times))
    else:
        out.write("  negative integers seen: none in this log\n")
    if asm.unsupported:
        out.write("  unsupported-format reports:\n")
        for _ln, text in asm.unsupported:
            out.write("    %s\n" % text)
    if asm.invalid:
        out.write("  records that failed the grammar: %d\n" % len(asm.invalid))
        for p in asm.invalid:
            out.write("    line %d: %s\n      record: %s\n" % (p.line_no, p.reason, safe(p.record, 200)))
    else:
        out.write("  records that failed the grammar: none\n")
    ok = not damaged and not asm.invalid and not asm.unsupported
    if not frames:
        out.write("  RESULT: no Curia frame found in this file\n")
        return 1
    out.write("  RESULT: %s\n" % ("all frames Complete, no grammar failures" if ok and not unfinished
                                  else "problems found, read the lines above"))
    return 0 if ok and not unfinished else 1


def analyse(data: bytes) -> Tuple[Assembler, int, int, bytes]:
    lines, tail = split_lines(data)
    asm = Assembler()
    overlong = 0
    for number, head, over in lines:
        if over:
            overlong += 1
        asm.feed(parse_line(head, over, number))
    return asm, len(lines), overlong, tail


def main(argv=None, out=None) -> int:
    ap = argparse.ArgumentParser(description="Check the CURIA1 records of a CK3 debug.log against docs/wire-format.md.")
    ap.add_argument("log", help="path of debug.log or a saved copy")
    ap.add_argument("--last", type=int, default=None, metavar="N",
                    help="print the tables of the last N frames only (the summary always covers all of them)")
    args = ap.parse_args(argv)
    if out is None:
        out = sys.stdout
        # Game names are not ASCII; a cp1252 console or a redirect under a plain locale must not kill the report.
        if hasattr(out, "reconfigure"):
            out.reconfigure(encoding="utf-8", errors="replace")
    try:
        with open(args.log, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        sys.stderr.write("curia1_check: cannot read %s: %s\n" % (args.log, exc.strerror))
        return 2
    asm, total, overlong, tail = analyse(data)
    return report(asm, total, overlong, tail, args.last, out)


if __name__ == "__main__":
    sys.exit(main())
