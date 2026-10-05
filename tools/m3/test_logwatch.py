# SPDX-License-Identifier: MIT
"""Unit tests for tools/m3/logwatch.py. Standard library only; uses synthetic data and temp folders.

Run: python3 -m unittest discover -s tools/m3 -v
The engine-style lines below are generated here (nothing is copied from a real game log).
Time is driven by FakeClock and explicit poll calls; no test depends on a wall-clock window. The threaded
real-time check is `python3 tools/m3/logwatch.py --selftest`, a manual command that is not part of this suite.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import signal
import tempfile
import time
import unittest
from unittest import mock
from typing import Any, List, Optional

import logwatch as lw


def local_wall(hh: int, mm: int, ss: int, frac: float = 0.0) -> float:
    """Wall-clock epoch seconds for a local time on a fixed day."""
    return time.mktime((2026, 10, 5, hh, mm, ss, 0, 0, -1)) + frac


def eline(clock: str, text: str, level: str = "D", source: str = "jomini_effect_impl.cpp:450") -> bytes:
    """A synthetic engine-style line; `clock` is 'HH:MM:SS'."""
    return f"[{clock}][{level}][{source}]: file: common/scripted_effects/x.txt line: 12 (effect): {text}\n".encode("utf-8")


class FakeClock(lw.Clock):
    def __init__(self, wall: float):
        self._wall = wall
        self._mono = 1000.0

    def wall(self) -> float:
        return self._wall

    def mono(self) -> float:
        return self._mono

    def sleep(self, seconds: float) -> None:
        self.advance(seconds)

    def advance(self, seconds: float) -> None:
        self._wall += seconds
        self._mono += seconds


class TempDirCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="curia-m3-test-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.logs = os.path.join(self.tmp, "logs")
        os.makedirs(self.logs)

    def path(self, name: str = "debug.log") -> str:
        return os.path.join(self.logs, name)

    def append(self, data: bytes, name: str = "debug.log") -> None:
        with open(self.path(name), "ab") as f:
            f.write(data)

    def make_watcher(self, wall: float = local_wall(12, 0, 0), **cfg_kw: Any) -> "tuple[lw.Watcher, FakeClock]":
        clock = FakeClock(wall)
        cfg = lw.Config(log_dir=self.logs, out_dir=os.path.join(self.tmp, "out"), report_every=0.0,
                        use_stdin=False, quiet=True, **cfg_kw)
        watcher = lw.Watcher(cfg, clock, lw.LabelState(clock, "none"), None, echo=lambda _t: None)
        return watcher, clock


class PrefixTests(unittest.TestCase):
    def test_parse(self) -> None:
        p = lw.parse_prefix(eline("13:05:09", "hello"))
        assert p is not None
        self.assertEqual(p.second_of_day, 13 * 3600 + 5 * 60 + 9)
        self.assertEqual(p.level, "D")
        self.assertEqual(p.source, "jomini_effect_impl.cpp:450")
        self.assertEqual(p.clock, "13:05:09")

    def test_no_prefix_or_bad_time(self) -> None:
        self.assertIsNone(lw.parse_prefix(b"plain text line"))
        self.assertIsNone(lw.parse_prefix(eline("25:00:00", "x")))
        self.assertIsNone(lw.parse_prefix(eline("12:61:00", "x")))

    def test_marker_found_anywhere(self) -> None:
        line = eline("00:00:01", "CURIA1|7|BEGIN|x")
        pos = lw.find_marker(line)
        self.assertGreater(pos, 40)
        self.assertTrue(line[pos:].startswith(b"CURIA1|7|BEGIN"))
        self.assertEqual(lw.find_marker(b"no marker here"), -1)


class BoundsTests(unittest.TestCase):
    def test_simple(self) -> None:
        lower, upper = lw.latency_bounds(12 * 3600 + 5.25, 12 * 3600 + 5)
        self.assertAlmostEqual(lower, -0.75)
        self.assertAlmostEqual(upper, 0.25)

    def test_later_arrival(self) -> None:
        lower, upper = lw.latency_bounds(100.0 + 3.5, 100)
        self.assertAlmostEqual(lower, 2.5)
        self.assertAlmostEqual(upper, 3.5)

    def test_midnight_rollover(self) -> None:
        # engine 23:59:59, arrival 00:00:00.5 the next day
        lower, upper = lw.latency_bounds(0.5, 86399)
        self.assertAlmostEqual(lower, 0.5)
        self.assertAlmostEqual(upper, 1.5)

    def test_engine_ahead_gives_negative_bounds_not_hidden(self) -> None:
        lower, upper = lw.latency_bounds(1000.0, 1010)
        self.assertAlmostEqual(upper, -10.0)
        self.assertAlmostEqual(lower, -11.0)

    def test_seconds_of_day(self) -> None:
        self.assertAlmostEqual(lw.seconds_of_day(local_wall(1, 2, 3, 0.5)), 3723.5, places=2)


class AssemblerTests(unittest.TestCase):
    def test_partial_line_across_reads(self) -> None:
        a = lw.LineAssembler()
        self.assertEqual(a.feed(b"first half "), [])
        self.assertEqual(a.feed(b"second"), [])
        lines = a.feed(b" half\nnext\n")
        self.assertEqual([ln.data for ln in lines], [b"first half second half", b"next"])
        self.assertEqual(lines[0].reads, 3)
        self.assertEqual(lines[1].reads, 1)
        self.assertEqual(lines[0].offset, 0)
        self.assertEqual(lines[1].offset, len(b"first half second half\n"))

    def test_crlf(self) -> None:
        lines = lw.LineAssembler().feed(b"one\r\ntwo\n")
        self.assertEqual([(ln.data, ln.crlf, ln.raw_len) for ln in lines], [(b"one", True, 5), (b"two", False, 4)])

    def test_new_line_started_in_same_read_counts_one(self) -> None:
        a = lw.LineAssembler()
        lines = a.feed(b"a\nb")
        self.assertEqual(len(lines), 1)
        more = a.feed(b"c\n")
        self.assertEqual(more[0].data, b"bc")
        self.assertEqual(more[0].reads, 2)

    def test_long_line(self) -> None:
        a = lw.LineAssembler()
        big = b"x" * 9000
        got: List[lw.Line] = []
        for i in range(0, len(big), 1000):
            got.extend(a.feed(big[i : i + 1000]))
        got.extend(a.feed(b"\n"))
        self.assertEqual(len(got), 1)
        self.assertEqual(len(got[0].data), 9000)
        self.assertEqual(got[0].reads, 10)

    def test_cap_flushes_oversized_partial(self) -> None:
        a = lw.LineAssembler(max_partial=100)
        lines = a.feed(b"y" * 150)
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].capped)

    def test_drop_head_when_attached_mid_line(self) -> None:
        a = lw.LineAssembler(start_offset=50, drop_head=True)
        self.assertEqual(a.feed(b"tail of a line"), [])
        lines = a.feed(b" more\nfull line\n")
        self.assertEqual([ln.data for ln in lines], [b"full line"])
        self.assertGreater(a.dropped_head_bytes, 0)

    def test_reset_returns_discarded(self) -> None:
        a = lw.LineAssembler()
        a.feed(b"unterminated")
        self.assertEqual(a.reset(), len(b"unterminated"))


class TailerTests(TempDirCase):
    def tailer(self, from_start: bool = False) -> lw.FileTailer:
        return lw.FileTailer(self.path(), "debug.log", FakeClock(local_wall(12, 0, 0)), from_start, 1 << 20)

    @staticmethod
    def kinds(events: List[Any]) -> List[str]:
        return [e.kind for e in events if isinstance(e, lw.TailFileEvent)]

    def test_missing_file_reports_waiting_once(self) -> None:
        t = self.tailer()
        self.assertEqual(self.kinds(t.poll()), ["waiting"])
        self.assertEqual(t.poll(), [])
        self.append(b"line one\n")
        events = t.poll()
        self.assertEqual(self.kinds(events), ["appeared"])
        lines = [e for e in events if isinstance(e, lw.TailLines)][0]
        self.assertEqual([ln.data for ln in lines.lines], [b"line one"])
        self.assertFalse(lines.backlog)

    def test_attach_skips_existing_content_by_default(self) -> None:
        self.append(b"old content\n")
        t = self.tailer()
        events = t.poll()
        self.assertEqual(self.kinds(events), ["attached"])
        self.assertEqual([e for e in events if isinstance(e, lw.TailLines)], [])
        self.append(b"new\n")
        got = [e for e in t.poll() if isinstance(e, lw.TailLines)]
        self.assertEqual([ln.data for ln in got[0].lines], [b"new"])

    def test_from_start_marks_backlog(self) -> None:
        self.append(b"old content\n")
        t = self.tailer(from_start=True)
        got = [e for e in t.poll() if isinstance(e, lw.TailLines)]
        self.assertTrue(got[0].backlog)
        self.append(b"new\n")
        got = [e for e in t.poll() if isinstance(e, lw.TailLines)]
        self.assertFalse(got[0].backlog)

    def test_attach_mid_line_drops_the_fragment(self) -> None:
        self.append(b"complete\nincomplete frag")
        t = self.tailer()
        t.poll()
        self.append(b"ment end\nwhole\n")
        got = [e for e in t.poll() if isinstance(e, lw.TailLines)]
        self.assertEqual([ln.data for ln in got[0].lines], [b"whole"])

    def test_partial_write_is_completed_on_a_later_poll(self) -> None:
        t = self.tailer()
        self.append(b"")
        t.poll()
        self.append(b"half a li")
        first = [e for e in t.poll() if isinstance(e, lw.TailLines)]
        self.assertEqual(first[0].lines, [])
        self.append(b"ne\n")
        second = [e for e in t.poll() if isinstance(e, lw.TailLines)]
        self.assertEqual(second[0].lines[0].data, b"half a line")
        self.assertEqual(second[0].lines[0].reads, 2)

    def test_truncation_same_inode(self) -> None:
        self.append(b"x" * 500 + b"\n")
        t = self.tailer()
        t.poll()
        self.append(b"more\n")
        t.poll()
        with open(self.path(), "r+b") as f:
            f.truncate(0)
        events = t.poll()
        ev = [e for e in events if isinstance(e, lw.TailFileEvent)][0]
        self.assertEqual((ev.kind, ev.old_size, ev.new_size), ("truncated", 506, 0))
        self.append(b"after launch\n")
        got = [e for e in t.poll() if isinstance(e, lw.TailLines)]
        self.assertEqual(got[0].lines[0].data, b"after launch")

    def test_recreated_with_smaller_size_new_inode(self) -> None:
        self.append(b"x" * 400 + b"\n")
        t = self.tailer()
        t.poll()
        os.rename(self.path(), self.path() + ".old")
        self.append(b"fresh\n")
        events = t.poll()
        ev = [e for e in events if isinstance(e, lw.TailFileEvent)][0]
        self.assertEqual((ev.kind, ev.old_size, ev.new_size), ("recreated", 401, 6))
        got = [e for e in events if isinstance(e, lw.TailLines)]
        self.assertEqual(got[0].lines[0].data, b"fresh")

    def test_recreated_larger_than_before_is_still_seen(self) -> None:
        self.append(b"short\n")
        t = self.tailer()
        t.poll()
        os.rename(self.path(), self.path() + ".old")
        self.append(b"a much longer replacement line\n")
        events = t.poll()
        self.assertEqual(self.kinds(events), ["recreated"])

    def test_disappeared_then_appeared(self) -> None:
        self.append(b"abc\n")
        t = self.tailer()
        t.poll()
        os.remove(self.path())
        self.assertEqual(self.kinds(t.poll()), ["disappeared"])
        self.assertEqual(t.poll(), [])
        self.append(b"back\n")
        self.assertEqual(self.kinds(t.poll()), ["appeared"])

    def test_bom_noted_when_read_from_first_byte(self) -> None:
        self.append(b"\xef\xbb\xbfhello\n")
        t = self.tailer(from_start=True)
        ev = [e for e in t.poll() if isinstance(e, lw.TailFileEvent)][0]
        self.assertTrue(ev.bom)


    def bom_events(self, events: List[Any]) -> List[lw.TailBom]:
        return [e for e in events if isinstance(e, lw.TailBom)]

    def test_bom_answered_by_a_later_poll_after_truncation(self) -> None:
        self.append(b"x" * 50 + b"\n")
        t = self.tailer()
        t.poll()
        with open(self.path(), "r+b") as f:
            f.truncate(0)
        events = t.poll()
        ev = [e for e in events if isinstance(e, lw.TailFileEvent)][0]
        self.assertEqual((ev.kind, ev.bom), ("truncated", None))
        self.assertEqual(self.bom_events(events), [])
        self.append(b"\xef\xbb\xbffirst\n")
        later = t.poll()
        self.assertEqual([b.bom for b in self.bom_events(later)], [True])
        self.append(b"more\n")
        self.assertEqual(self.bom_events(t.poll()), [])  # answered once

    def test_bom_answered_by_a_later_poll_after_appearing_empty(self) -> None:
        t = self.tailer()
        t.poll()
        self.append(b"")
        self.assertEqual(self.kinds(t.poll()), ["appeared"])
        self.append(b"no bom here\n")
        self.assertEqual([b.bom for b in self.bom_events(t.poll())], [False])

    def test_bom_split_across_two_reads(self) -> None:
        t = self.tailer()
        t.poll()
        self.append(b"")
        t.poll()
        self.append(b"\xef\xbb")
        self.assertEqual(self.bom_events(t.poll()), [])  # three bytes are needed to tell
        self.append(b"\xbfrest\n")
        self.assertEqual([b.bom for b in self.bom_events(t.poll())], [True])

    def test_bom_in_same_poll_sets_the_event(self) -> None:
        t = self.tailer()
        t.poll()
        self.append(b"\xef\xbb\xbfhello\n")
        events = t.poll()
        ev = [e for e in events if isinstance(e, lw.TailFileEvent)][0]
        self.assertEqual((ev.kind, ev.bom), ("appeared", True))
        self.assertEqual(self.bom_events(events), [])


class WatcherTests(TempDirCase):
    def start(self, **kw: Any) -> "tuple[lw.Watcher, FakeClock]":
        # create the files first so the watcher attaches at their end
        self.append(b"")
        self.append(b"", "error.log")
        w, clock = self.make_watcher(**kw)
        w.poll_once()
        return w, clock

    def frame_lines(self, snap: str, clock: str, body: int = 2, declared: Optional[int] = None) -> bytes:
        out = eline(clock, f"CURIA1|{snap}|BEGIN|1066.9.15|1")
        for i in range(body):
            out += eline(clock, f"CURIA1|{snap}|ROW{i}|v{i}")
        n = declared if declared is not None else body + 2
        out += eline(clock, f"CURIA1|{snap}|END|{n}")
        return out

    def test_sample_fields_and_bounds(self) -> None:
        w, clock = self.start()
        clock.advance(0.0)
        wall = local_wall(12, 0, 3, 0.4)
        clock._wall = wall
        w.labels.apply_line("paused")
        self.append(eline("12:00:02", "CURIA1|a|BEGIN|1066.9.15|1"))
        w.poll_once()
        s = w.analysis.samples[0]
        self.assertEqual(s.label, "paused")
        self.assertEqual(s.engine_sod, 12 * 3600 + 2)
        assert s.lower is not None and s.upper is not None
        self.assertAlmostEqual(s.lower, 0.4, places=2)
        self.assertAlmostEqual(s.upper, 1.4, places=2)
        self.assertEqual(s.reads, 1)

    def test_late_bom_answer_reaches_the_summary_after_truncation(self) -> None:
        w, clock = self.start()
        self.append(eline("12:00:01", "plain"))
        w.poll_once()
        with open(self.path(), "r+b") as f:
            f.truncate(0)
        clock.advance(1)
        w.poll_once()
        self.append(b"\xef\xbb\xbf" + eline("12:00:02", "plain"))
        clock.advance(1)
        w.poll_once()
        ev = [e for e in w.analysis.file_events if e["event"] == "truncated"][0]
        self.assertIs(ev["bom"], True)
        self.assertIn("UTF-8 BOM at start True", w.finish())

    def test_engine_offset_keeps_a_fractional_part(self) -> None:
        w, clock = self.start(engine_offset_s=-0.5)
        clock._wall = local_wall(12, 0, 3, 0.4)
        self.append(eline("12:00:02", "CURIA1|a|BEGIN|d|1"))
        w.poll_once()
        s = w.analysis.samples[0]
        assert s.lower is not None and s.upper is not None
        self.assertAlmostEqual(s.engine_sod or 0.0, 12 * 3600 + 1.5, places=3)
        self.assertAlmostEqual(s.lower, 0.9, places=2)
        self.assertAlmostEqual(s.upper, 1.9, places=2)
        w2, clock2 = self.make_watcher(engine_offset_s=0.5)  # attaches at the end of the file written above
        w2.poll_once()
        clock2._wall = local_wall(12, 0, 3, 0.4)
        self.append(eline("12:00:02", "CURIA1|b|BEGIN|d|1"))
        w2.poll_once()
        assert w2.analysis.samples[0].upper is not None
        self.assertAlmostEqual(w2.analysis.samples[0].upper, 0.9, places=2)

    def test_engine_offset_option_shifts_bounds(self) -> None:
        w, clock = self.start(engine_offset_s=-3600.0)
        clock._wall = local_wall(12, 0, 3, 0.5)
        self.append(eline("13:00:02", "CURIA1|a|BEGIN|d|1"))
        w.poll_once()
        s = w.analysis.samples[0]
        assert s.upper is not None
        self.assertAlmostEqual(s.upper, 1.5, places=2)

    def test_complete_frame_and_interleaving(self) -> None:
        w, clock = self.start()
        data = (
            eline("12:00:01", "CURIA1|f1|BEGIN|1066.9.15|1")
            + eline("12:00:01", "some other engine output")
            + eline("12:00:01", "CURIA1|f1|ROW|a")
            + eline("12:00:01", "CURIA1|f2|BEGIN|1066.9.15|2")
            + eline("12:00:01", "CURIA1|f1|END|3")
            + eline("12:00:01", "CURIA1|f2|END|2")
        )
        self.append(data)
        w.poll_once()
        frames = {f["snap_id"]: f for f in w.analysis.frames_done}
        self.assertTrue(frames["f1"]["complete"])
        self.assertEqual(frames["f1"]["lines"], 3)
        self.assertEqual(frames["f1"]["interleaved_noncuria_lines"], 1)
        self.assertEqual(frames["f1"]["interleaved_other_frame_lines"], 1)
        self.assertTrue(frames["f2"]["complete"])
        expected = sum(len(x) for x in (b"CURIA1|f1|BEGIN|1066.9.15|1", b"CURIA1|f1|ROW|a", b"CURIA1|f1|END|3"))
        self.assertEqual(frames["f1"]["payload_bytes"], expected)
        self.assertGreater(frames["f1"]["raw_bytes_with_prefix"], frames["f1"]["payload_bytes"])

    def test_end_count_mismatch_is_reported(self) -> None:
        w, clock = self.start()
        self.append(self.frame_lines("m1", "12:00:01", body=2, declared=9))
        w.poll_once()
        f = w.analysis.frames_done[0]
        self.assertFalse(f["complete"])
        self.assertEqual(f["declared_count"], 9)
        self.assertIn("frame_count_mismatch", w.analysis.anomaly_counts)

    def test_body_convention(self) -> None:
        w, clock = self.start(count_convention="body")
        self.append(self.frame_lines("b1", "12:00:01", body=2, declared=2))
        w.poll_once()
        self.assertTrue(w.analysis.frames_done[0]["complete"])
        self.assertTrue(w.analysis.frames_done[0]["declared_matches_body_lines"])
        self.assertFalse(w.analysis.frames_done[0]["declared_matches_all_lines"])

    def test_partial_frame_reported_at_finish(self) -> None:
        w, clock = self.start()
        self.append(eline("12:00:01", "CURIA1|p1|BEGIN|d|1") + eline("12:00:01", "CURIA1|p1|ROW|a"))
        w.poll_once()
        text = w.finish()
        self.assertIn("partial_frame", w.analysis.anomaly_counts)
        self.assertFalse(w.analysis.frames_done[0]["complete"])
        self.assertIn("no END", text)

    def test_duplicate_line_and_line_after_end(self) -> None:
        w, clock = self.start()
        data = self.frame_lines("d1", "12:00:01", body=1) + eline("12:00:01", "CURIA1|d1|ROW0|v0") + eline("12:00:01", "CURIA1|d1|LATE|z")
        self.append(data)
        w.poll_once()
        self.assertEqual(w.analysis.anomaly_counts["duplicate_line"], 1)
        self.assertEqual(w.analysis.anomaly_counts["line_after_end"], 2)

    def test_long_line_over_eight_thousand_characters(self) -> None:
        w, clock = self.start()
        self.append(eline("12:00:01", "CURIA1|L|LONG|" + "x" * 8200))
        w.poll_once()
        s = w.analysis.samples[0]
        self.assertGreater(s.line_bytes, 8200)
        self.assertEqual(w.analysis.anomaly_counts["long_line"], 1)

    def test_line_arriving_in_two_reads(self) -> None:
        w, clock = self.start()
        full = eline("12:00:01", "CURIA1|q|BEGIN|d|1")
        self.append(full[:50])
        w.poll_once()
        self.assertEqual(w.analysis.samples, [])
        clock.advance(0.2)
        self.append(full[50:])
        w.poll_once()
        self.assertEqual(w.analysis.samples[0].reads, 2)

    def test_crlf_lines_are_handled(self) -> None:
        w, clock = self.start()
        self.append(eline("12:00:01", "CURIA1|c|BEGIN|d|1").replace(b"\n", b"\r\n"))
        w.poll_once()
        self.assertEqual(len(w.analysis.samples), 1)
        self.assertEqual(w.analysis.crlf_lines["debug.log"], 1)
        self.assertEqual(w.analysis.samples[0].line_bytes, len(eline("12:00:01", "CURIA1|c|BEGIN|d|1")) - 1)

    def test_raw_file_keeps_the_terminator_that_was_seen(self) -> None:
        self.append(b"")
        clock = FakeClock(local_wall(12, 0, 0))
        cfg = lw.Config(log_dir=self.logs, out_dir=os.path.join(self.tmp, "out"), report_every=0.0, use_stdin=False, quiet=True)
        rec = lw.Recorder(cfg.out_dir, "term")
        w = lw.Watcher(cfg, clock, lw.LabelState(clock), rec, echo=lambda _t: None)
        w.poll_once()
        lf = eline("12:00:00", "CURIA1|t|BEGIN|d|1")
        crlf = eline("12:00:00", "CURIA1|t|ROW|a").replace(b"\n", b"\r\n")
        self.append(lf + crlf + lf)
        w.poll_once()
        w.finish()
        with open(rec.raw_path, "rb") as f:
            self.assertEqual(f.read(), lf + crlf + lf)

    def test_invalid_utf8_and_non_ascii(self) -> None:
        w, clock = self.start()
        self.append(eline("12:00:01", "CURIA1|u|NAME|").rstrip(b"\n") + b"caf\xc3\xa9 and bad \xff\xfe\n")
        self.append(b"plain other line with bad \xff byte\n")
        w.poll_once()
        counts = w.analysis.anomaly_counts
        self.assertEqual(counts["invalid_utf8"], 1)
        self.assertEqual(counts["non_ascii_or_control"], 1)
        bad = [a for a in w.analysis.anomalies if a["kind"] == "non_ascii_or_control"][0]
        self.assertIn("\ufffd", bad["text"])
        self.assertGreater(len(bad["offsets"]), 0)
        self.assertEqual(w.analysis.invalid_utf8_other["debug.log"], 1)

    def test_error_log_hits_case_insensitive(self) -> None:
        w, clock = self.start()
        self.append(eline("12:00:00", "unrelated", "E", "x.cpp:1"), "error.log")
        self.append(eline("12:00:00", "variable in CURIA_M3 set but never used", "E", "x.cpp:1"), "error.log")
        self.append(eline("12:00:00", "bad key in mod/Curia/common/x.txt", "E", "x.cpp:1"), "error.log")
        w.poll_once()
        self.assertEqual(len(w.analysis.error_hits), 2)
        self.assertIn("CURIA_M3", w.analysis.error_hits[0]["text"])
        self.assertIn("CURIA_M3", w.finish())

    def test_error_log_pattern_does_not_match_curia_inside_other_words(self) -> None:
        w, clock = self.start()
        for word in ("Mercurial", "curial", "Curiae", "curiate"):
            self.append(eline("12:00:00", f"{word} text", "E", "x.cpp:1"), "error.log")
        w.poll_once()
        self.assertEqual(w.analysis.error_hits, [])
        self.assertIn("matching the mod pattern 'curia[_/]'", w.finish())

    def test_mod_pattern_is_configurable(self) -> None:
        w, clock = self.start(mod_pattern=r"curia_m3\b")
        self.append(eline("12:00:00", "curia_m3_x", "E", "x.cpp:1"), "error.log")
        self.append(eline("12:00:00", "CURIA_M3 set", "E", "x.cpp:1"), "error.log")
        self.append(eline("12:00:00", "curia/other", "E", "x.cpp:1"), "error.log")
        w.poll_once()
        self.assertEqual(len(w.analysis.error_hits), 1)
        self.assertIn("CURIA_M3", w.analysis.error_hits[0]["text"])
        self.assertIn("mod pattern 'curia_m3\\b'", w.finish())

    def test_error_pattern_is_not_applied_to_debug_log(self) -> None:
        w, clock = self.start()
        self.append(eline("12:00:00", "curia_thing in debug"))
        w.poll_once()
        self.assertEqual(w.analysis.error_hits, [])

    def test_budget_separates_curia_lines(self) -> None:
        w, clock = self.start()
        other = eline("12:00:01", "plain engine output")
        curia = eline("12:00:01", "CURIA1|z|BEGIN|d|1")
        self.append(other + curia + other)
        w.poll_once()
        tot = w.analysis.budget_total["debug.log"]
        self.assertEqual(tot["other"], 2 * len(other))
        self.assertEqual(tot["curia"], len(curia))

    def test_rates_use_the_active_window_not_the_idle_time(self) -> None:
        w, clock = self.start()
        clock.advance(480)  # eight idle minutes: the game has not started writing
        first = eline("12:08:00", "plain engine output") * 10
        self.append(first)
        w.poll_once()
        clock.advance(600)
        second = eline("12:18:00", "plain engine output") * 10
        self.append(second)
        w.poll_once()
        self.assertAlmostEqual(w.analysis.active_window_s(), 600.0, places=3)
        text = w.finish()
        total = len(first) + len(second)
        self.assertIn("active window (first appended byte to the latest one) 10.0 min", text)
        self.assertIn(lw.fmt_bytes(total / 10.0), text)  # per minute over the active window
        self.assertIn(lw.fmt_bytes(total * 6.0), text)  # per hour, shown because the window is 10 minutes
        # the minute buckets share the origin: the second append falls into minute 10, the first into minute 0
        self.assertEqual(sorted(w.analysis.budget_minutes["debug.log"]), [0, 10])

    def test_truncation_event_recorded_with_sizes_and_time(self) -> None:
        w, clock = self.start()
        self.append(eline("12:00:01", "plain") * 20)
        w.poll_once()
        size = os.path.getsize(self.path())
        with open(self.path(), "r+b") as f:
            f.truncate(0)
        clock.advance(5)
        w.poll_once()
        ev = [e for e in w.analysis.file_events if e["type"] == "truncation"][0]
        self.assertEqual((ev["old_size"], ev["new_size"]), (size, 0))
        self.assertIn("12:00:05", ev["time"])

    def test_new_session_resets_frame_state(self) -> None:
        for how in ("truncated", "recreated", "disappeared"):
            with self.subTest(how=how):
                w, clock = self.make_watcher()
                if os.path.exists(self.path()):
                    os.remove(self.path())
                self.append(b"")
                w.poll_once()
                begin_and_row = eline("12:00:01", "CURIA1|g1|BEGIN|1066.9.15|1") + eline("12:00:01", "CURIA1|g1|ROW0|v0")
                frame = begin_and_row + eline("12:00:01", "CURIA1|g1|END|3")
                self.append(begin_and_row)  # no END: the frame is open when the game restarts
                w.poll_once()
                self.assertEqual(len(w.analysis.open_frames), 1)
                if how == "truncated":
                    with open(self.path(), "r+b") as f:
                        f.truncate(0)
                elif how == "recreated":
                    os.rename(self.path(), self.path() + ".old")
                    self.append(b"")
                else:
                    os.remove(self.path())
                clock.advance(1)
                w.poll_once()
                self.assertEqual(w.analysis.open_frames, {})
                self.assertEqual(w.analysis.anomaly_counts["partial_frame"], 1)
                self.assertFalse(w.analysis.frames_done[0]["complete"])
                self.assertEqual(w.analysis.frames_done[0]["session"], 1)
                if how == "disappeared":
                    self.append(b"")
                    w.poll_once()
                # the same game date and ids again after the restart: no duplicate reports
                self.append(frame)
                clock.advance(1)
                w.poll_once()
                self.assertEqual(w.analysis.anomaly_counts["duplicate_line"], 0)
                self.assertEqual(w.analysis.anomaly_counts["duplicate_snap_id"], 0)
                self.assertEqual(w.analysis.anomaly_counts["line_after_end"], 0)
                self.assertTrue(w.analysis.frames_done[-1]["complete"])
                self.assertEqual(w.analysis.frames_done[-1]["session"], 2)

    def test_waiting_for_debug_log_is_reported(self) -> None:
        said: List[str] = []
        clock = FakeClock(local_wall(12, 0, 0))
        cfg = lw.Config(log_dir=self.logs, out_dir=self.tmp, report_every=0.0, use_stdin=False, quiet=True)
        w = lw.Watcher(cfg, clock, lw.LabelState(clock), None, echo=said.append)
        w.poll_once()
        self.assertTrue(any("waiting for debug.log" in s for s in said))

    def test_clock_offset_is_reported_not_hidden(self) -> None:
        w, clock = self.start()
        clock._wall = local_wall(12, 0, 0, 0.5)
        self.append(eline("12:00:30", "plain") + eline("12:00:31", "plain"))
        w.poll_once()
        text = w.finish()
        self.assertIn("apparent offset", text)
        self.assertIn("AHEAD", text.upper())

    def test_summary_contains_all_sections(self) -> None:
        w, clock = self.start()
        w.labels.apply_line("fast")
        clock._wall = local_wall(12, 0, 7, 0.5)
        self.append(self.frame_lines("s1", "12:00:05", body=2))
        w.poll_once()
        text = w.finish()
        for needle in (
            "1. Files", "2. Flush latency bounds per label", "label 'fast'", "lower bound s (min / median / max)",
            "upper bound s", "what the bounds permit", "3. CURIA line format", "4. Frames", "5. Truncation",
            "6. error.log lines matching the mod pattern", "7. Byte budget", "8. Engine clock", "9. Anomalies", "s1 | 4 | 4",
        ):
            self.assertIn(needle, text)
        self.assertNotIn("estimate", text.lower().replace("not a forecast", ""))

    def test_outputs_written(self) -> None:
        self.append(b"")
        clock = FakeClock(local_wall(12, 0, 0))
        cfg = lw.Config(log_dir=self.logs, out_dir=os.path.join(self.tmp, "out"), report_every=0.0, use_stdin=False, quiet=True)
        rec = lw.Recorder(cfg.out_dir, "test")
        w = lw.Watcher(cfg, clock, lw.LabelState(clock), rec, echo=lambda _t: None)
        w.poll_once()
        line = eline("12:00:00", "CURIA1|o|BEGIN|d|1")
        self.append(line)
        w.poll_once()
        w.finish()
        with open(rec.raw_path, "rb") as f:
            self.assertEqual(f.read(), line)
        with open(rec.jsonl_path, encoding="utf-8") as f:
            kinds = [json.loads(x)["type"] for x in f]
        for t in ("start", "file", "sample", "frame", "budget", "end"):
            self.assertIn(t, kinds)
        self.assertTrue(os.path.exists(rec.summary_path))


class LabelTests(unittest.TestCase):
    def test_labels_and_notes(self) -> None:
        clock = FakeClock(local_wall(12, 0, 0))
        state = lw.LabelState(clock, "none")
        state.apply_line("paused\n")
        state.apply_line("   \n")
        state.apply_line("!clicked advisor twice\n")
        self.assertEqual(state.current(), "paused")
        events = state.drain()
        self.assertEqual([e["type"] for e in events], ["label", "note"])
        self.assertEqual(events[1]["text"], "clicked advisor twice")

    def test_reader_thread_from_a_pipe(self) -> None:
        clock = lw.Clock()
        state = lw.LabelState(clock, "none")
        r, w = os.pipe()
        reader = os.fdopen(r, "r")
        writer = os.fdopen(w, "w")
        thread = lw.start_label_reader(reader, state)
        writer.write("menu\n!note one\nfast\n")
        writer.close()  # end of input: the reader thread finishes after handling every line
        thread.join(timeout=30)
        self.assertFalse(thread.is_alive())
        self.assertEqual(state.current(), "fast")
        kinds = [e["type"] for e in state.drain()]
        self.assertEqual(kinds, ["label", "note", "label"])
        reader.close()


class RunAndMainTests(TempDirCase):
    def test_interrupt_does_one_last_poll_and_reraises(self) -> None:
        self.append(b"")
        self.append(b"", "error.log")
        outer = self

        class InterruptingClock(FakeClock):
            def sleep(self, seconds: float) -> None:
                outer.append(eline("12:00:01", "CURIA1|i|BEGIN|d|1"))  # arrives just before the interrupt
                raise KeyboardInterrupt

        clock = InterruptingClock(local_wall(12, 0, 2))
        cfg = lw.Config(log_dir=self.logs, out_dir=self.tmp, report_every=0.0, use_stdin=False, quiet=True)
        w = lw.Watcher(cfg, clock, lw.LabelState(clock), None, echo=lambda _t: None)
        with self.assertRaises(KeyboardInterrupt):
            w.run()
        self.assertEqual(len(w.analysis.samples), 1)

    def test_summary_is_written_when_the_run_fails(self) -> None:
        out = os.path.join(self.tmp, "out")
        argv = ["--log-dir", self.logs, "--out-dir", out, "--no-stdin"]
        before = self.handlers()
        with mock.patch.object(lw.Watcher, "run", side_effect=RuntimeError("boom")):
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                code = lw.main(argv)
        self.assertEqual(code, 1)
        self.assertEqual(self.handlers(), before)  # the signal handlers were restored
        names = os.listdir(out)
        self.assertTrue(any(n.startswith("summary-") for n in names), names)

    def test_summary_is_written_on_interrupt(self) -> None:
        out = os.path.join(self.tmp, "out")
        argv = ["--log-dir", self.logs, "--out-dir", out, "--no-stdin"]
        with mock.patch.object(lw.Watcher, "run", side_effect=KeyboardInterrupt):
            with contextlib.redirect_stdout(io.StringIO()):
                code = lw.main(argv)
        self.assertEqual(code, 0)
        self.assertTrue(any(n.startswith("summary-") for n in os.listdir(out)))

    @staticmethod
    def handlers() -> "dict[int, Any]":
        sigs = [signal.SIGINT, signal.SIGTERM] + ([signal.SIGHUP] if hasattr(signal, "SIGHUP") else [])
        return {sig: signal.getsignal(sig) for sig in sigs}

    def test_invalid_mod_pattern_is_rejected(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                lw.main(["--mod-pattern", "(", "--no-stdin", "--out-dir", self.tmp])


if __name__ == "__main__":
    unittest.main()
