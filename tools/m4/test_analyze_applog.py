# SPDX-License-Identifier: MIT
"""Unit tests for tools/m4/analyze_applog.py (synthetic app logs only).

Run: python3 -m unittest discover -s tools/m4 -v
"""
from __future__ import annotations

import contextlib
import datetime as dt
import io
import os
import tempfile
import unittest

import analyze_applog as aa

START = dt.datetime(2026, 10, 20, 18, 0, 0, tzinfo=dt.timezone.utc)


class Log:
    """Builds app-log lines in AppLog's format at chosen times (seconds after START)."""

    def __init__(self):
        self.lines = []
        self.t = 0.0

    def at(self, seconds):
        self.t = float(seconds)
        return self

    def add(self, event, level="info", **fields):
        moment = START + dt.timedelta(seconds=self.t)
        stamp = moment.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (moment.microsecond // 1000)
        parts = [stamp, level, event]
        for key, value in fields.items():
            if isinstance(value, bool):
                value = "true" if value else "false"
            parts.append("%s=%s" % (key, value))
        self.lines.append(" ".join(parts))
        return self

    def text(self):
        return "\n".join(self.lines) + "\n"

    # A whole window run's start, as the app writes it.
    def window_start(self, focus="key", tray="created"):
        self.add("dev_config", key="set", model="set")
        self.add("overlay_policy", before="prohibited", switched=False, policy="accessory")
        self.add("provider_set", persona_version=4, effort="low", thinking="default")
        self.add("window_start", pid=4242, configured=True, log_found=True, log_from_flag=False, window="overlay",
                 focus=focus)
        self.add("controls", tray=tray, hotkey="registered")
        return self

    def reading(self, reading, snap, date, gold, income, mode=0, shown_after_ms=None, basis="game"):
        self.add("frame_complete", snap=snap, mode=mode, records=21, text_shortened=False, snapshot_bytes=3100,
                 reading=reading, game_date=date, gold=gold, income_tenths=income)
        if shown_after_ms is not None:
            self.t += shown_after_ms / 1000.0
            self.add("overlay_placed", basis=basis, display=1, displays=2, game_window=True, x=1500, y=60, w=520,
                     h=680)
            self.add("overlay_shown", kind="panel", reason="game_click", focus="key", panel_key=True,
                     app_active=False, ck3_front="yes")
            self.add("overlay_opened", source="game_click")
        return self

    def ask(self, gen, reading, first_ms=900, total_ms=3000, cache_read=0, cache_write=0, connects=0, end="done",
            stop_after_ms=None, code="Overloaded"):
        self.add("chat_request", body_bytes=9000, messages=1, test_endpoint=False, auth="bearer")
        self.add("ask_started", gen=gen, persona_version=4, persona_bytes=2767, snapshot_bytes=3100, messages=1,
                 conversation_bytes=20, snapshot_age_s=5, unreadable=False, reading=reading)
        base = self.t
        self.t = base + first_ms / 1000.0
        self.add("ask_first_token", gen=gen, first_token_ms=first_ms)
        if end == "done":
            self.t = base + total_ms / 1000.0
            self.add("chat_done", input=40, output=300, cache_read=cache_read, cache_write=cache_write,
                     first_token_ms=first_ms, total_ms=total_ms, stop_reason="end_turn")
            self.add("ask_done", gen=gen, input=40, output=300, cache_read=cache_read, cache_write=cache_write,
                     max_tokens=False, first_token_ms=first_ms, total_ms=total_ms, answer_bytes=900)
            self.add("net_done", outcome="finished", status=200, curl=0, bytes_in=5000, bytes_out=9000,
                     connect_ms=40, first_byte_ms=500, total_ms=total_ms, tls_retry=False, http="1.1",
                     connects=connects, conn="released")
        elif end == "stopped":
            self.t = base + stop_after_ms / 1000.0
            self.add("ask_cancelled", gen=gen, total_ms=stop_after_ms, answer_bytes=120)
            self.t += 0.004
            self.add("net_done", outcome="cancelled", status=200, curl=0, bytes_in=2000, bytes_out=9000,
                     connect_ms=40, first_byte_ms=500, total_ms=stop_after_ms, tls_retry=False, http="1.1",
                     connects=connects, conn="closed")
            self.add("chat_error", level="warn", code="Cancelled", retryable=False, http_status=0,
                     partial_bytes=120, total_ms=stop_after_ms)
        else:
            self.t = base + total_ms / 1000.0
            self.add("net_done", outcome="finished", status=529, curl=0, bytes_in=100, bytes_out=9000,
                     connect_ms=40, first_byte_ms=500, total_ms=total_ms, tls_retry=False, http="1.1",
                     connects=connects, conn="released")
            self.add("chat_error", level="warn", code=code, retryable=True, http_status=529, partial_bytes=0,
                     total_ms=total_ms)
            self.add("ask_error", level="warn", gen=gen, code=code, status=529, retryable=True, total_ms=total_ms,
                     answer_bytes=0)
        return self


def analyse_text(text, timeline=False):
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "curia.log")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
        return aa.analyse([path], utc=True, timeline=timeline)


def section(output, title):
    start = output.index("## " + title)
    end = output.find("\n## ", start + 3)
    return output[start:end if end >= 0 else len(output)]


class ParseTests(unittest.TestCase):
    def test_fields_plain_quoted_and_bare(self):
        fields = aa.parse_fields(' a=1 b="two words" c="x\\"y\\\\z" [redacted] d= e=end')
        self.assertEqual(fields, {"a": "1", "b": "two words", "c": 'x"y\\z', "d": "", "e": "end"})

    def test_a_line_and_its_time(self):
        line = aa.parse_line("2026-10-20T18:00:01.250Z info frame_complete snap=7 gold=-3", 0, 5)
        self.assertIsNotNone(line)
        self.assertEqual(line.ms - int(START.timestamp() * 1000), 1250)
        self.assertEqual((line.event, line.num("snap"), line.num("gold"), line.number), ("frame_complete", 7, -3, 5))
        self.assertIsNone(aa.parse_line("not a log line"))
        self.assertIsNone(aa.parse_line("2026-13-40T18:00:01.250Z info x"))

    def test_words_are_the_apps_own_or_other(self):
        line = aa.parse_line("2026-10-20T18:00:01.250Z info overlay_shown reason=hotkey focus=Zorblax")
        self.assertEqual(line.word("reason"), "hotkey")
        self.assertEqual(line.word("focus"), "other")
        self.assertEqual(line.word("missing"), "-")
        self.assertEqual(line.word("anything_unknown"), "-")

class RunTests(unittest.TestCase):
    def test_runs_start_at_dev_config_after_an_exit_and_when_the_counts_restart(self):
        log = Log().window_start()
        log.at(10).add("log_bytes", why="minute", read=500, curia=0, since_truncation=500, truncations=0)
        log.at(20).add("log_bytes", why="exit", read=600, curia=0, since_truncation=600, truncations=0)
        log.at(30).add("provider_set", persona_version=4, effort="low", thinking="default")  # a headless run
        log.at(40).add("log_bytes", why="minute", read=900, curia=0, since_truncation=900, truncations=0)
        log.at(50).add("log_bytes", why="minute", read=100, curia=0, since_truncation=100, truncations=0)
        log.at(60).window_start(focus="click")
        runs = aa.split_runs(aa.parse_line(l, 0, i) for i, l in enumerate(log.lines, 1))
        self.assertEqual(len(runs), 4)
        self.assertTrue(runs[0].exited)
        self.assertEqual(runs[3].first("window_start").word("focus"), "click")
        output, _ = analyse_text(log.text())
        self.assertIn("| 1 | 2026-10-20 18:00 | 18:00:20 | 0.3 | overlay | key | set | set | created | registered | "
                      "accessory | low | default | 4 | yes |", output)

    def test_the_lines_of_the_shutdown_after_the_exit_line_stay_with_their_run(self):
        # Quit while an answer streams: the controller stops it after log_bytes why=exit, and the
        # request ends on the net thread a few milliseconds later.
        log = Log().window_start()
        log.at(5).reading(1, 81, "1071-11-27", 928, 78, shown_after_ms=4)
        log.at(10).add("chat_request", body_bytes=9000, messages=1, test_endpoint=False, auth="bearer")
        log.add("ask_started", gen=1, persona_version=4, persona_bytes=2767, snapshot_bytes=3100, messages=1,
                conversation_bytes=20, snapshot_age_s=5, unreadable=False, reading=1)
        log.at(12).add("log_bytes", why="exit", read=4000, curia=3100, since_truncation=4000, truncations=0,
                       mailbox_dropped=0)
        log.at(12.001).add("ask_cancelled", gen=1, total_ms=2001, answer_bytes=40)
        log.at(12.004).add("net_done", outcome="cancelled", status=200, curl=0, bytes_in=900, bytes_out=9000,
                           connect_ms=40, first_byte_ms=500, total_ms=2004, tls_retry=False, http="1.1", connects=1,
                           conn="closed")
        log.add("chat_error", level="warn", code="Cancelled", retryable=False, http_status=0, partial_bytes=40,
                total_ms=2004)
        log.at(30).add("provider_set", persona_version=4, effort="low", thinking="default")  # the next run
        runs = aa.split_runs(aa.parse_line(l, 0, i) for i, l in enumerate(log.lines, 1))
        self.assertEqual(len(runs), 2)
        self.assertEqual([l.event for l in runs[0].lines[-3:]], ["ask_cancelled", "net_done", "chat_error"])
        output, _ = analyse_text(log.text())
        self.assertIn("| 1 | 18:00:12 | 2001 | 40 | 3 | cancelled | closed | 1.1 | - | - |", section(output, "Stops"))


class ReadingTests(unittest.TestCase):
    def test_readings_with_the_time_until_the_overlay_was_shown(self):
        log = Log().window_start()
        log.at(100).reading(1, 81, "1071-11-27", 928, 78, shown_after_ms=4)
        log.at(200).reading(2, 82, "1071-11-27", 931, 78, mode=1, shown_after_ms=12, basis="pointer")
        log.at(300).reading(3, 83, "1071-11-28", 935, 78)  # no overlay line (a headless run, say)
        output, code = analyse_text(log.text())
        self.assertEqual(code, 0)
        part = section(output, "Readings")
        self.assertIn("| 1 | 1 | 18:01:40 | 1 | 81 | Advisor | 1071-11-27 | 928 | 78 | 21 | 4 | game |", part)
        self.assertIn("| 2 | 1 | 18:03:20 | 2 | 82 | Curia probes | 1071-11-27 | 931 | 78 | 21 | 12 | pointer |", part)
        self.assertIn("| 3 | 1 | 18:05:00 | 3 | 83 | Advisor | 1071-11-28 | 935 | 78 | 21 | - | - |", part)
        self.assertIn("Readings: 3; the overlay was shown for 2 of them (median 8 ms, longest 12 ms", part)

    def test_none_for_a_frame_without_treasury(self):
        log = Log().window_start()
        log.at(5).add("frame_complete", snap=9, mode=0, records=2, text_shortened=False, snapshot_bytes=200,
                      reading=1, game_date="1067-10-01", gold="none", income_tenths="none")
        log.at(6).add("frame_complete", snap=10, mode=0, records=2, text_shortened=False, snapshot_bytes=200,
                      reading=2, game_date="1067-13-40", gold=5, income_tenths=7)  # not a date
        part = section(analyse_text(log.text())[0], "Readings")
        self.assertIn("| 1067-10-01 | - | - |", part)
        self.assertIn("| 10 | Advisor | - | 5 | 7 |", part)


class IncomeTests(unittest.TestCase):
    def income(self, readings):
        log = Log().window_start()
        for i, (date, income) in enumerate(readings, 1):
            log.at(60 * i).reading(i, i, date, 100, income)
        return section(analyse_text(log.text())[0], "S3")

    def test_the_hypothesis_holds_when_every_change_is_at_a_boundary(self):
        part = self.income([("1071-11-27", 78), ("1071-11-28", 78), ("1071-11-30", 78), ("1071-12-01", 81),
                            ("1071-12-02", 81), ("1072-01-01", 81)])
        self.assertIn("| 1071-11-30 | 1071-12-01 | 1 | 78 | 81 | yes | yes | 1 |", part)
        self.assertIn("| 1071-12-02 | 1072-01-01 | 30 | 81 | 81 | no | yes | - |", part)  # not a step of the test
        self.assertIn("| 1 | 1071-11-27 | 1071-12-02 | 5 | 0 | 1 | 0 | holds |", part)
        self.assertIn("All readings: changes within a month: 0; across a month boundary: 1; boundaries without a "
                      "change: 1; date went back (a save loaded?): 0.", part)
        self.assertIn("holds for these readings", part)

    def test_it_does_not_hold_when_the_income_moves_within_a_month(self):
        part = self.income([("1071-11-27", 78), ("1071-11-28", 79), ("1071-12-01", 80)])
        self.assertIn("does not hold: the income changed within a month 1 time(s)", part)
        self.assertIn("| 1 | 1071-11-27 | 1071-12-01 | 3 | 1 | 1 | 0 | does not hold |", part)

    def test_not_shown_when_the_income_never_changed(self):
        part = self.income([("1071-11-27", 78), ("1071-11-28", 78), ("1071-11-30", 78), ("1071-12-01", 78),
                            ("1071-12-02", 78)])
        self.assertIn("| 1 | 1071-11-27 | 1071-12-02 | 5 | 0 | 0 | 1 | not shown |", part)
        self.assertIn("not shown: the income did not change at a month boundary (nor within a month)", part)
        self.assertNotIn("holds", part)

    def test_a_verdict_per_series(self):
        # An earlier stretch of play (other questions, the game running for a while) with a change
        # within a month, then the steps of the test across a boundary: the series of the test
        # holds; the overall line still counts the earlier change.
        part = self.income([("1071-10-03", 70), ("1071-10-15", 74), ("1071-11-27", 78), ("1071-11-28", 78),
                            ("1071-11-29", 78), ("1071-11-30", 78), ("1071-12-01", 81), ("1071-12-02", 81)])
        self.assertIn("| 1071-10-03 | 1071-10-15 | 12 | 70 | 74 | yes | no | - |", part)
        self.assertIn("| 3 | 1071-11-27 | 1071-12-02 | 6 | 0 | 1 | 0 | holds |", part)
        self.assertNotIn("| 1 | 1071-10-03 |", part)  # a single reading is no series
        self.assertIn("does not hold: the income changed within a month 1 time(s)", part)

    def test_not_tested_without_a_boundary_and_a_date_that_went_back(self):
        part = self.income([("1071-11-27", 78), ("1071-11-28", 78)])
        self.assertIn("not tested: no month boundary", part)
        part = self.income([("1071-11-27", 78), ("1070-05-01", 60), ("1070-05-02", 60)])
        self.assertIn("| 1071-11-27 | 1070-05-01 | -575 | 78 | 60 | yes | date went back | - |", part)
        self.assertIn("date went back (a save loaded?): 1", part)

    def test_the_game_calendar_has_no_leap_years(self):
        self.assertEqual(aa.game_day((1072, 3, 1)) - aa.game_day((1072, 2, 28)), 1)
        self.assertEqual(aa.game_day((1072, 1, 1)) - aa.game_day((1071, 12, 31)), 1)
        self.assertIsNone(aa.game_day((1072, 2, 29)))
        self.assertIsNone(aa.game_day((1072, 13, 1)))


class AskTests(unittest.TestCase):
    def build(self):
        log = Log().window_start()
        log.at(100).reading(1, 81, "1071-11-27", 928, 78, shown_after_ms=4)
        log.at(110).ask(1, 1, first_ms=1003, total_ms=4008, cache_write=5200, connects=1)
        log.at(150).ask(2, 1, first_ms=950, total_ms=3500, cache_read=5200, connects=0)
        log.at(200).ask(3, 1, first_ms=900, end="stopped", stop_after_ms=2100, connects=0)
        log.at(230).ask(5, 1, first_ms=1100, total_ms=3900, connects=1)
        log.at(600).ask(6, 1, first_ms=1000, total_ms=3000, connects=0)  # more than 5 minutes later
        log.at(700).reading(2, 82, "1071-12-01", 940, 81, shown_after_ms=5)
        log.at(710).ask(7, 2, first_ms=900, total_ms=3000, connects=0)  # another reading
        log.at(720).ask(8, 2, end="error", total_ms=300, code="Overloaded")
        log.at(730).add("ask_refused", outcome="Busy")
        return log

    def test_the_questions_table(self):
        output, _ = analyse_text(self.build().text())
        part = section(output, "Questions")
        self.assertIn("| 1 | 1 | 18:01:50 | 1 | 1 | 3100 | 5 | 1003 | 4008 | 40 | 300 | 5200 | 0 | end_turn | done | "
                      "200 | 1.1 | 1 | released |", part)
        self.assertIn("| 3 | 1 | 18:03:20 | 3 | 1 | 3100 | 5 | 900 | 2100 | - | - | - | - | - | stopped | 200 | 1.1 "
                      "| 0 | closed |", part)
        self.assertIn("| error Overloaded | 529 | 1.1 | 0 | released |", part)
        self.assertIn("Answered: 5 of 7", part)
        self.assertIn("Refused (nothing was sent): Busy 1.", part)

    def test_the_cache_pairs(self):
        part = section(analyse_text(self.build().text())[0], "Cache")
        self.assertIn("| 2 | 1 | 40.0 | 1 | 5200 | 0 | read from the cache |", part)
        self.assertIn("| 5 | 3 | 30.0 | 1 | 0 | 0 | no cache read |", part)
        self.assertNotIn("| 6 | 5 |", part)  # 370 s apart
        self.assertNotIn("| 7 | 6 |", part)  # another reading
        self.assertIn("| 8 | 7 | 10.0 | 2 | - | - | no answer |", part)
        self.assertIn("Pairs: 4; with cache_read > 0: 1.", part)

    def test_the_stops(self):
        part = section(analyse_text(self.build().text())[0], "Stops")
        self.assertIn("| 3 | 18:03:22 | 2100 | 120 | 4 | cancelled | closed | 1.1 | 27.9 | 1 |", part)
        self.assertIn("the client's classification", part)

    def test_the_request_line_written_before_ask_started_belongs_to_it(self):
        log = Log().window_start()
        log.at(10).ask(1, 0, connects=1)
        log.at(20).add("chat_request", body_bytes=10, messages=1, test_endpoint=False, auth="bearer")  # check-api
        asks = aa.collect_asks(aa.split_runs(aa.parse_line(l) for l in log.lines)[0])
        self.assertEqual(len(asks), 1)
        self.assertEqual(asks[0].request.number, 0)
        self.assertEqual(asks[0].net.num("connects"), 1)


class BytesTests(unittest.TestCase):
    def test_per_hour_while_ck3_ran(self):
        log = Log().window_start()
        log.add("ck3_running", running=False, poll_us=90)
        log.at(600).add("ck3_running", running=True, poll_us=6000)
        log.at(601).add("log_truncated")
        read = curia = 0
        # 150 minutes of play: 12,000 bytes a minute of the game's own lines, one Advisor click
        # (4,000 bytes) every 10 minutes.
        for minute in range(1, 151):
            read += 12000
            if minute % 10 == 0:
                read += 4000
                curia += 4000
                log.at(600 + minute * 60 - 1).reading(minute // 10, minute // 10, "1071-11-27", 1, 1)
            log.at(600 + minute * 60).add("log_bytes", why="minute", read=read, curia=curia,
                                          since_truncation=read, truncations=1)
        log.at(600 + 150 * 60 + 5).add("ck3_running", running=False, poll_us=7000)
        read += 3000  # the game's last lines, reported by the next minute line
        log.at(600 + 150 * 60 + 40).add("log_bytes", why="minute", read=read, curia=curia, since_truncation=read,
                                        truncations=1)
        read += 500  # long after CK3 quit (not counted)
        log.at(600 + 150 * 60 + 600).add("log_bytes", why="exit", read=read, curia=curia, since_truncation=read,
                                         truncations=1)
        output, _ = analyse_text(log.text())
        part = section(output, "S4")
        self.assertIn("| 1 | 1 | 1 | 18:10:00 | 19:10:00 | 60.0 | 744000 | 24000 | 720000 | 744000 | 720000 | 6 | "
                      "yes |", part)
        self.assertIn("| 1 | 1 | 2 | 19:10:00 | 20:10:00 | 60.0 | 744000 | 24000 | 720000 | 744000 | 720000 | 6 | "
                      "no |", part)
        self.assertIn("| 1 | 1 | 3 | 20:10:00 | 20:40:05 | 30.1 | 375000 | 12000 | 363000 | 747922 | 723989 | 3 | "
                      "no |", part)
        self.assertIn("Overall while CK3 ran: 150.1 minutes, 1863000 bytes (60000 of them Curia's lines, 15 readings)",
                      part)
        self.assertIn("Bytes read while CK3 was not running (not counted above): 500.", part)
        # Play: from the count before the first reading (18:19:00, minute 9) to CK3's quit; the
        # nine minutes of launch before it are left out (108,000 bytes).
        self.assertIn("| 1 | 1 | 18:10:00 | 20:40:05 | 150.1 | yes | 18:19:59 | 108000 | 141.1 | 1755000 | 60000 | "
                      "746367 | 720851 |", part)
        self.assertIn("Play only (without CK3's launch and the loading of a save): 141.1 minutes, 1755000 bytes "
                      "(60000 of them Curia's lines); per hour 746367, without Curia's lines 720851.", part)

    def test_short_stretches_get_no_rate_and_a_run_without_the_poll_is_flagged(self):
        log = Log()
        log.add("provider_set", persona_version=4, effort="low", thinking="default")
        log.at(60).add("log_bytes", why="minute", read=1000, curia=100, since_truncation=1000, truncations=0)
        log.at(120).add("log_bytes", why="exit", read=1500, curia=100, since_truncation=1500, truncations=0)
        part = section(analyse_text(log.text())[0], "S4")
        self.assertIn("| 1 | 1 | 1 | 18:00:00 | 18:02:00 | 2.0 | 1500 | 100 | 1400 | - | - | 0 | no |", part)
        self.assertIn("Runs without a ck3_running line (the whole run is counted as CK3 running): 1.", part)

    def test_a_restart_of_ck3_within_the_minute_gives_each_stretch_its_own_bytes(self):
        log = Log().window_start()
        log.at(0).add("ck3_running", running=True, poll_us=100)
        log.at(60).add("log_bytes", why="minute", read=1000, curia=0, since_truncation=1000, truncations=0)
        log.at(100).add("ck3_running", running=False, poll_us=100)
        log.at(110).add("ck3_running", running=True, poll_us=100)
        log.at(111).add("log_truncated")
        # 20 s after the first stretch ended, but inside the second: the second stretch's bytes.
        log.at(120).add("log_bytes", why="minute", read=1600, curia=0, since_truncation=600, truncations=1)
        log.at(140).add("ck3_running", running=False, poll_us=100)
        # Within a minute after both stretches ended: the later one's last bytes.
        log.at(150).add("log_bytes", why="exit", read=1700, curia=0, since_truncation=700, truncations=1)
        part = section(analyse_text(log.text())[0], "S4")
        self.assertIn("| 1 | 1 | 1 | 18:00:00 | 18:01:40 | 1.7 | 1000 | 0 | 1000 | - | - | 0 | no |", part)
        self.assertIn("| 1 | 2 | 1 | 18:01:50 | 18:02:20 | 0.5 | 700 | 0 | 700 | - | - | 0 | yes |", part)
        # CK3 was running when Curia started (no truncation): all of the first stretch is play;
        # the second one was launched and saw no reading: launch only.
        self.assertIn("| 1 | 1 | 18:00:00 | 18:01:40 | 1.7 | no | - | - | 1.7 | 1000 | 0 | - | - |", part)
        self.assertIn("| 1 | 2 | 18:01:50 | 18:02:20 | 0.5 | yes | - | 700 | - | - | - | - | - |", part)
        self.assertIn("Stretches in which CK3 was launched that ended before any play was counted: run 1 stretch 2.",
                      part)

    def test_without_a_reading_play_counts_from_ten_minutes_after_the_launch(self):
        log = Log().window_start()
        log.add("ck3_running", running=False, poll_us=90)
        log.at(60).add("ck3_running", running=True, poll_us=6000)
        log.at(60.4).add("log_truncated")
        read = 0
        for minute in range(1, 31):
            read += 300000 if minute <= 3 else 10000  # the launch and a save loading, then play
            log.at(60 + minute * 60).add("log_bytes", why="minute", read=read, curia=0, since_truncation=read,
                                         truncations=1)
        log.at(60 + 30 * 60 + 1).add("ck3_running", running=False, poll_us=6000)
        part = section(analyse_text(log.text())[0], "S4")
        # The count at minute 10 is the last of the launch; play: minutes 11 to 30, 20 x 10,000.
        self.assertIn("| 1 | 1 | 18:01:00 | 18:31:01 | 30.0 | yes | - | 970000 | 20.0 | 200000 | 0 | 599500 | "
                      "599500 |", part)
        self.assertIn("no reading came (play counted from 10 minutes after the launch): run 1 stretch 1.", part)

    def test_the_launch_lines_just_before_ck3_running_belong_to_its_stretch(self):
        # The order the app may write at a launch: CK3 truncates its log and writes its first
        # lines before the poll (once a second) sees the process.
        log = Log().window_start()
        log.add("ck3_running", running=False, poll_us=90)
        log.at(100.2).add("log_truncated")
        log.at(100.5).add("log_bytes", why="minute", read=600000, curia=0, since_truncation=600000, truncations=1)
        log.at(100.506).add("ck3_running", running=True, poll_us=6000)
        read = 600000
        for minute in range(1, 121):
            read += 10000
            if minute == 5:
                log.at(100.5 + minute * 60 - 1).reading(1, 1, "1071-11-27", 1, 1)
            log.at(100.5 + minute * 60).add("log_bytes", why="minute", read=read, curia=0, since_truncation=read,
                                            truncations=1)
        log.at(100.5 + 120 * 60 + 1).add("log_bytes", why="exit", read=read, curia=0, since_truncation=read,
                                         truncations=1)
        part = section(analyse_text(log.text())[0], "S4")
        self.assertNotIn("not running (not counted above)", part)
        self.assertIn("| 1 | 1 | 1 | 18:01:40 | 19:01:40 | 60.0 | 1200000 | 0 | 1200000 | 1200000 | 1200000 | 1 | yes |",
                      part)
        # Play from the count at minute 4 (before the first reading) on: 116 minutes of 10,000.
        self.assertIn("| 1 | 1 | 18:01:40 | 20:01:41 | 120.0 | yes | 18:06:39 | 640000 | 116.0 | 1160000 | 0 | "
                      "599914 | 599914 |", part)

    def test_short_sessions_still_give_a_play_figure(self):
        # Three evenings of 50 minutes each: no hour without a launch, but play after each first
        # reading.
        log = Log()
        t = 0
        for evening in range(3):
            log.at(t).window_start()
            log.at(t + 1).add("ck3_running", running=False, poll_us=90)
            log.at(t + 10).add("ck3_running", running=True, poll_us=6000)
            log.at(t + 10.5).add("log_truncated")
            read = 0
            for minute in range(1, 51):
                read += 500000 if minute == 1 else 10000
                if minute == 3:
                    log.at(t + 10 + minute * 60 - 1).reading(evening + 1, evening + 1, "1071-11-27", 1, 1)
                log.at(t + 10 + minute * 60).add("log_bytes", why="minute", read=read, curia=0,
                                                 since_truncation=read, truncations=1)
            log.at(t + 10 + 50 * 60 + 2).add("ck3_running", running=False, poll_us=7000)
            log.at(t + 10 + 50 * 60 + 3).add("log_bytes", why="exit", read=read, curia=0, since_truncation=read,
                                             truncations=1)
            t += 24 * 3600
        part = section(analyse_text(log.text())[0], "S4")
        self.assertIn("Play only (without CK3's launch and the loading of a save): 144.1 minutes, 1440000 bytes (0 of "
                      "them Curia's lines); per hour 599584, without Curia's lines 599584.", part)

    def test_messages_dropped_by_the_mailbox_are_reported(self):
        log = Log().window_start()
        log.at(60).add("log_bytes", why="minute", read=1000, curia=0, since_truncation=1000, truncations=0,
                       mailbox_dropped=0)
        log.at(120).add("log_bytes", why="exit", read=1500, curia=0, since_truncation=1500, truncations=0,
                        mailbox_dropped=7)
        part = section(analyse_text(log.text())[0], "S4")
        self.assertIn("Messages the app's mailbox dropped (its limit; a reading may be among them), by run: run 1: 7.",
                      part)
        log = Log().window_start()
        log.at(60).add("log_bytes", why="exit", read=1000, curia=0, since_truncation=1000, truncations=0,
                       mailbox_dropped=0)
        self.assertNotIn("mailbox dropped", section(analyse_text(log.text())[0], "S4"))

    def test_no_log_bytes(self):
        part = section(analyse_text(Log().window_start().text())[0], "S4")
        self.assertIn("No log_bytes line", part)


class OverlayTests(unittest.TestCase):
    def test_counts_of_the_overlay_lines(self):
        log = Log().window_start()
        log.add("overlay_placed", basis="pointer", display=0, displays=2, game_window=False, x=1, y=2, w=3, h=4)
        log.add("overlay_shown", kind="panel", reason="start", focus="click", panel_key=False, app_active=False,
                ck3_front="no")
        log.at(0.05).add("overlay_focus", why="focus_gained", kind="panel", visible=True, panel_key=True,
                         app_active=True, ck3_front="no")
        log.at(0.25).add("overlay_keys", after="start", returned="remembered", to="other", accepted=True)
        log.at(30).reading(1, 5, "1071-11-27", 1, 1, shown_after_ms=3)
        log.at(30.1).add("overlay_focus", why="first_key", kind="panel", visible=True, panel_key=True,
                         app_active=False, ck3_front="yes")
        log.at(40).add("overlay_hidden", kind="panel", app_active=False)
        log.at(40.25).add("overlay_keys", after="hide", returned="not_needed")
        log.at(41).add("overlay_focus", why="settled_hide", kind="panel", visible=False, panel_key=False,
                       app_active=False, ck3_front="yes")
        log.at(50).add("overlay_shown", kind="panel", reason="hotkey", focus="key", panel_key=True,
                       app_active=False, ck3_front="yes")
        log.add("overlay_opened", source="hotkey")
        log.add("overlay_shown", kind="panel", reason="tray", focus="Zorblax", panel_key=True, app_active=False,
                ck3_front="yes")
        output, _ = analyse_text(log.text(), timeline=True)
        part = section(output, "Overlay")
        self.assertIn("| game_click | key | true | false | yes | 1 |", part)
        self.assertIn("| hotkey | key | true | false | yes | 1 |", part)
        self.assertIn("| start | click | false | false | no | 1 |", part)
        self.assertIn("| tray | other | true | false | yes | 1 |", part)
        self.assertIn("| first_key | true | true | false | yes | 1 |", part)
        self.assertIn("| start | remembered | other | true | 1 |", part)
        self.assertIn("| hide | not_needed | - | - | 1 |", part)
        self.assertIn("| game | 1 | 2 | true | 1 |", part)
        self.assertIn("| pointer | 0 | 2 | false | 1 |", part)
        self.assertIn("Hidden (overlay_hidden): 1, of them with Curia active: 0.", part)
        self.assertNotIn("Zorblax", output)
        timeline = section(output, "Timeline")
        self.assertIn("| 1 | 18:00:30.100 | overlay_focus | why=first_key visible=true panel_key=true app_active=false "
                      "ck3_front=yes |", timeline)

    def test_a_headless_log_with_only_opened_lines(self):
        log = Log()
        log.add("provider_set", persona_version=4, effort="low", thinking="default")
        log.at(5).add("overlay_opened", source="refresh")
        part = section(analyse_text(log.text())[0], "Overlay")
        self.assertIn("Shown (overlay_shown):\n\nNone.", part)
        self.assertIn("Placed (overlay_placed):\n\nNone.", part)
        self.assertIn("| refresh | 1 |", part)

    def test_ck3_and_notices(self):
        log = Log().window_start()
        log.add("ck3_running", running=True, poll_us=100)
        log.at(5).add("notice", name="None")
        log.at(70).add("ck3_running", running=False, poll_us=6000)
        log.add("notice", name="Ck3NotRunning")
        part = section(analyse_text(log.text())[0], "CK3 and the notices")
        self.assertIn("| 1 | 18:01:10 | ck3_running | false |", part)
        self.assertIn("| 1 | 18:01:10 | notice | Ck3NotRunning |", part)


class KeyCheckTests(unittest.TestCase):
    def test_key_shapes_are_counted_never_shown(self):
        log = Log().window_start()
        log.add("odd", value="sk-ant-FAKEabcdefgh0123")
        log.add("odd2", value="Ab1" + "x" * 40)
        log.add("chat_request", body_bytes=1, messages=1, test_endpoint=False, auth="bearer")
        log.lines.append("garbage line with [redacted] in it")
        output, code = analyse_text(log.text())
        self.assertEqual(code, 1)
        part = section(output, "Key check")
        self.assertIn("FOUND: 2 line(s) shaped like an API key (file:line): 1:6, 1:7.", part)
        self.assertIn("[redacted] before writing (file:line): 1:9.", part)
        self.assertNotIn("FAKEabcdefgh", output)
        self.assertNotIn("xxxxxxxx", output)
        self.assertIn("| 1 | 9 | 8 | 1 | yes |", section(output, "Files"))

    def test_a_clean_log(self):
        log = Log().window_start()
        log.at(10).ask(1, 0)
        output, code = analyse_text(log.text())
        self.assertEqual(code, 0)
        self.assertIn("No line shaped like an API key", output)
        self.assertNotIn("[redacted] before writing", output)


class MainTests(unittest.TestCase):
    def test_a_folder_is_read_as_the_two_generations_in_order(self):
        with tempfile.TemporaryDirectory() as folder:
            old = Log().window_start()
            old.at(10).reading(1, 1, "1071-01-01", 1, 1)
            new = Log()
            new.at(20).reading(2, 2, "1071-02-01", 1, 2)
            with open(os.path.join(folder, "curia.log.1"), "w") as handle:
                handle.write(old.text())
            with open(os.path.join(folder, "curia.log"), "w") as handle:
                handle.write(new.text())
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(aa.main([folder, "--utc"]), 0)
            text = out.getvalue()
            self.assertIn("| 1 | 6 | 6 | 0 | yes |", text)
            self.assertIn("| 2 | 1 | 1 | 0 | yes |", text)
            self.assertIn("| 1071-01-01 | 1071-02-01 | 31 | 1 | 2 | yes | yes | - |", text)

    def test_usage_errors(self):
        err = io.StringIO()
        with tempfile.TemporaryDirectory() as folder, contextlib.redirect_stderr(err):
            self.assertEqual(aa.main([os.path.join(folder, "missing.log")]), 2)
            self.assertEqual(aa.main([folder]), 2)  # a folder without curia.log
            # No path: a usage error, never a default folder (the owner's own app log is read only
            # when named on the command line).
            self.assertEqual(aa.main([]), 2)
            self.assertEqual(aa.main(["--bogus"]), 2)
        self.assertIn("no app log found", err.getvalue())

    @unittest.skipIf(not hasattr(os, "geteuid") or os.geteuid() == 0, "needs file permissions (not root)")
    def test_an_unreadable_log_is_a_usage_error_and_a_missing_one_is_listed(self):
        with tempfile.TemporaryDirectory() as folder:
            locked = os.path.join(folder, "curia.log")
            with open(locked, "w") as handle:
                handle.write(Log().window_start().text())
            readable = os.path.join(folder, "other.log")
            with open(readable, "w") as handle:
                handle.write(Log().window_start().text())
            os.chmod(locked, 0)
            try:
                out, err = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    self.assertEqual(aa.main([locked]), 2)
                self.assertEqual(out.getvalue(), "")
                self.assertIn("no app log could be read", err.getvalue())
                out = io.StringIO()
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(aa.main([locked, os.path.join(folder, "gone.log"), readable, "--utc"]), 0)
            finally:
                os.chmod(locked, 0o600)
            text = out.getvalue()
            self.assertIn("| 1 | 0 | 0 | 0 | no |", text)
            self.assertIn("| 2 | 0 | 0 | 0 | does not exist |", text)
            self.assertIn("| 3 | 5 | 5 | 0 | yes |", text)
            self.assertIn("Not checked: file(s) 1, 2 could not be read", text)

    def test_junk_lines_and_an_internal_error(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "curia.log")
            with open(path, "w") as handle:
                handle.write("0001-01-01T00:00:00.000Z info dev_config key=set model=set\n")
                handle.write(Log().window_start().text())
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(aa.main([path]), 0)  # local time: a year 1 line would not convert
            self.assertIn("| 1 | 6 | 5 | 1 | yes |", out.getvalue())
            original = aa.analyse

            def broken(*args, **kwargs):
                raise ValueError("a line that holds sk-ant-FAKE-text-0000")
            aa.analyse = broken
            self.addCleanup(setattr, aa, "analyse", original)
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.assertEqual(aa.main([path]), 2)
            self.assertEqual(out.getvalue(), "")
            self.assertIn("internal error", err.getvalue())
            self.assertNotIn("FAKE", err.getvalue())


if __name__ == "__main__":
    unittest.main()
