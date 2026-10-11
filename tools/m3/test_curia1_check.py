# SPDX-License-Identifier: MIT
"""Tests for curia1_check.py. Artificial names only; the engine-style prefix is built here at test time."""
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest

import curia1_check as cc


def prefix(i=0):
    # Built from parts so that no engine-style line is stored in a tracked file.
    return "[{:02d}:{:02d}:{:02d}][{}][{}] file: {} line: {} ({}): ".format(
        12, 0, i % 60, "D", "jomini_script_system.cpp:285", "common/x.txt", 3, "test_effect").encode()


def physical(record: str, i=0) -> bytes:
    return prefix(i) + record.encode("utf-8") + b"\n"


EXAMPLE = [
    "CURIA1|7|BEGIN|0|0.1.0|1066|9|15|1001|Test Ruler One",
    "CURIA1|7|FIN|312|78",
    "CURIA1|7|HEIR|1002|84|1|0|Test Heir Two",
    "CURIA1|7|VASSALS|2|2",
    "CURIA1|7|VASSAL|1|1003|120|55|1523|Test Vassal Three",
    "CURIA1|7|VASSAL|2|1004|40|-12|310|Test Vassal Four",
    "CURIA1|7|WARS|0",
    "CURIA1|7|CLAIMS|1|0|1",
    "CURIA1|7|CLAIM|1|weak|4|Test Realm",
    "CURIA1|7|FACTIONS|1|0|1",
    "CURIA1|7|FACTION|1|INFO|against|3|33|100|2",
    "CURIA1|7|FACTION|1|LEADER|1004|Test Vassal Four",
    "CURIA1|7|FACTION|1|CLAIMANT|1005|Test Claimant Five",
    "CURIA1|7|FACTION|1|TITLE|4|Test Realm",
    "CURIA1|7|FACTION|1|TARGET|1001|Test Ruler One",
    "CURIA1|7|END|16",
]


def run(records, raw_lines=None):
    """Feeds records (strings) as engine-prefixed lines and returns the Assembler."""
    data = b"".join(physical(r, i) for i, r in enumerate(records))
    if raw_lines:
        data += raw_lines
    asm, _total, _over, _tail = cc.analyse(data)
    return asm


def parse(record: str, overlong=False):
    return cc.parse_line(prefix() + record.encode("utf-8"), overlong)


def kinds(asm):
    return [(k, r) for k, _s, r in asm.events]


class ParseLineTests(unittest.TestCase):
    def test_not_curia_lines(self):
        for line in (b"", b"some other engine line", prefix() + b"no marker here", b"CURIA1|7|FIN|1|2"):
            self.assertEqual(cc.parse_line(line).kind, "none", line)

    def test_marker_at_offset_zero_is_refused_even_if_a_later_one_exists(self):
        self.assertEqual(cc.parse_line(b"CURIA1|7|FIN|1|2 and again CURIA1|7|FIN|1|2").kind, "none")

    def test_marker_after_one_byte_is_a_record(self):
        self.assertEqual(cc.parse_line(b"xCURIA1|7|FIN|1|2").kind, "record")

    def test_prefix_is_not_part_of_the_record(self):
        p = parse("CURIA1|7|FIN|312|78")
        self.assertEqual(p.record, b"CURIA1|7|FIN|312|78")
        self.assertEqual((p.type, p.snap, p.values), ("FIN", 7, {"gold": 312, "income": 78}))

    def assert_invalid(self, record, fragment, overlong=False):
        p = parse(record, overlong)
        self.assertEqual(p.kind, "invalid", record[:120])
        self.assertIn(fragment, p.reason, record[:120])
        return p

    def test_worked_invalid_records_of_the_spec(self):
        self.assert_invalid("CURIA1|7|FIN|312", "too few fields")
        self.assert_invalid("CURIA1|7|FIN|312|9|1", "too many fields")
        self.assert_invalid("CURIA1|7|FIN|312|9\r", "non-ASCII byte")
        self.assert_invalid("CURIA1|7|FIN|007|9", "leading zeros")
        self.assert_invalid("CURIA1|7|FIN|+5|9", "plus sign")
        self.assert_invalid("CURIA1|7|FIN||9", "empty")
        self.assert_invalid("CURIA1|7|FIN|1e3|9", "not an int")
        self.assert_invalid("CURIA1|7|END|99999999999999999999", "does not fit int32")
        self.assert_invalid("CURIA1|7|HEIR|1002|84|1|0", "text delimiter missing")
        self.assert_invalid("CURIA1|7|HEIR|none|", "bytes after `none`")
        self.assert_invalid("CURIA1|7|FACTION|1|LEADER|1004", "text delimiter missing")
        self.assert_invalid("CURIA1|7|CLAIM|1|Weak|4|x", "case-sensitive")
        self.assert_invalid("CURIA1|7|BEGIN|0|1.0 0|1066|9|15|1001|x", "token")
        self.assert_invalid("CURIA1|7|fin|1|2", "type")
        self.assert_invalid("CURIA1|7|FACTION|1|TITLE|none|", "bytes after `none`")
        self.assert_invalid("CURIA1|7|FACTION|1", "without a sub-kind")
        self.assert_invalid("CURIA1|0|BEGIN|0|0.1.0|1066|9|15|1|x", "snap 0 is valid only on LOAD")
        self.assert_invalid("CURIA1|7|VASSALS|-0|0", "-0 is valid only in a signed field")

    def test_header_failures(self):
        for rec in ("CURIA1|", "CURIA1||BEGIN", "CURIA1|abc|FOO", "CURIA1|7", "CURIA1|7|", "CURIA1|-5|FIN|1|2",
                    "CURIA1|-0|FIN|1|2", "CURIA1|00|FIN|1|2", "CURIA1|2147483648|FIN|1|2", "CURIA1|7|Fin|1|2"):
            self.assertEqual(parse(rec).kind, "invalid", rec)

    def test_valid_and_notable_records(self):
        self.assertEqual(parse("CURIA1|7|HEIR|1002|84|1|0|").text, b"")
        self.assertTrue(parse("CURIA1|7|HEIR|none").none)
        p = parse("CURIA1|7|FIN|-0|-0")
        self.assertEqual((p.kind, p.values), ("record", {"gold": 0, "income": 0}))
        self.assertEqual([n for _f, n in p.negatives], ["-0", "-0"])
        u = parse("CURIA1|7|FACTION|2|DISCONTENT|42")
        self.assertEqual((u.kind, u.unknown, u.sub), ("record", True, "DISCONTENT"))

    def test_text_may_contain_pipes_and_cannot_move_a_field(self):
        p = parse("CURIA1|7|VASSAL|1|1003|120|55|1523|Name|with|pipes")
        self.assertEqual(p.text, b"Name|with|pipes")
        self.assertEqual(p.values["strength"], 1523)

    def test_ranges(self):
        for rec in ("CURIA1|7|BEGIN|2|0.1.0|1066|9|15|1|x", "CURIA1|7|BEGIN|0|0.1.0|10000|9|15|1|x",
                    "CURIA1|7|BEGIN|0|0.1.0|1066|0|15|1|x", "CURIA1|7|BEGIN|0|0.1.0|1066|13|15|1|x",
                    "CURIA1|7|BEGIN|0|0.1.0|1066|9|0|1|x", "CURIA1|7|BEGIN|0|0.1.0|1066|9|32|1|x",
                    "CURIA1|7|BEGIN|0|0.1.0|1066|9|15|-1|x", "CURIA1|7|VASSAL|0|1|1|1|1|x",
                    "CURIA1|7|CLAIM|0|weak|1|x", "CURIA1|7|CLAIM|1|weak|6|x", "CURIA1|7|CLAIM|1|weak|0|x",
                    "CURIA1|7|FACTION|1|INFO|against|8|1|1|1", "CURIA1|7|FACTION|1|INFO|against|0|1|1|1",
                    "CURIA1|7|FACTION|0|INFO|against|1|1|1|1", "CURIA1|7|FACTIONS|1|2|1",
                    "CURIA1|7|HEIR|1|1|2|0|x", "CURIA1|7|FIN|2147483648|1", "CURIA1|7|FIN|-2147483649|1"):
            self.assertEqual(parse(rec).kind, "invalid", rec)
        self.assertEqual(parse("CURIA1|7|BEGIN|0|0.1.0|1066|2|31|1|x").kind, "record")  # impossible date accepted
        self.assertEqual(parse("CURIA1|7|FIN|-2147483648|2147483647").kind, "record")

    def test_negative_integers_are_recorded_with_their_printed_form(self):
        p = parse("CURIA1|7|VASSAL|1|1003|-5|-12|1523|x")
        self.assertEqual(p.negatives, [("VASSAL.gold", "-5"), ("VASSAL.opinion", "-12")])

    def test_non_ascii_minus_is_invalid_and_named(self):
        p = self.assert_invalid("CURIA1|7|FIN|−5|9", "non-ASCII byte")
        self.assertIn("e2 88 92", p.reason)

    def test_non_ascii_in_a_non_text_field_is_invalid(self):
        self.assertEqual(parse("CURIA1|7|CLAIM|1|ströng|4|x").kind, "invalid")

    def test_snap_zero_rules(self):
        self.assertEqual(parse("CURIA1|0|LOAD|0.2.0").kind, "record")
        self.assertEqual(parse("CURIA1|1|LOAD|0.2.0").kind, "invalid")
        self.assertEqual(parse("CURIA1|0|END|1").kind, "invalid")
        self.assertEqual(parse("CURIA1|0|PROBE|x").kind, "invalid")  # an unknown type is not exempt from step 3
        self.assertEqual(parse("CURIA1|0|LOAD|bad token").kind, "invalid")

    def test_unknown_types_only_check_the_header(self):
        for rec in ("CURIA1|7|PROBE|anything \x01 é | at all", "CURIA1|7|FUTURE", "CURIA1|7|FUTURE|1|2|\x07"):
            p = parse(rec)
            self.assertEqual((p.kind, p.unknown), ("record", True), rec)
        self.assertEqual(parse("CURIA1|7|FACTION|1|DISCONTENT").unknown, True)
        self.assertEqual(parse("CURIA1|7|WAR|3|SCORE|x\x01").unknown, True)

    def test_unknown_subkind_header_rules(self):
        for rec in ("CURIA1|7|WAR|0|SCORE|1", "CURIA1|7|WAR|x|SCORE|1", "CURIA1|7|WAR|1|", "CURIA1|7|WAR|1|score|1",
                    "CURIA1|7|FACTION", "CURIA1|7|FACTION|1|"):
            self.assertEqual(parse(rec).kind, "invalid", rec)

    def test_unknown_record_is_exempt_from_the_oversize_rule(self):
        p = parse("CURIA1|7|PROBE|" + "x" * 3000)
        self.assertEqual((p.kind, p.unknown, p.shortened), ("record", True, False))

    def test_oversize_text_is_shortened_and_valid(self):
        p = parse("CURIA1|7|VASSAL|1|1003|120|55|1523|" + "a" * 3000)
        self.assertEqual((p.kind, p.shortened, p.length), ("record", True, 1024))
        self.assertEqual(p.values["strength"], 1523)
        self.assertEqual(len(p.text), 1024 - len(b"CURIA1|7|VASSAL|1|1003|120|55|1523|"))

    def test_oversize_cut_never_leaves_half_a_character(self):
        head = "CURIA1|7|VASSAL|1|1003|120|55|1523|"
        pad = 1023 - len(head)  # the first byte of a 2-byte character lands on byte 1024
        p = parse(head + "a" * pad + "é" + "b" * 50)
        self.assertEqual(p.length, 1023)
        self.assertEqual(p.text.decode("utf-8"), "a" * pad)
        # a 3-byte character cut after two bytes: both bytes are dropped
        pad3 = 1022 - len(head)
        p = parse(head + "a" * pad3 + "€" + "b" * 50)
        self.assertEqual(p.length, 1022)
        # a complete character at the cut is kept
        pad2 = 1022 - len(head)
        p = parse(head + "a" * pad2 + "é" + "b" * 50)
        self.assertEqual(p.length, 1024)

    def test_oversize_with_fixed_fields_beyond_the_limit_is_invalid(self):
        p = self.assert_invalid("CURIA1|7|BEGIN|0|" + "v" * 2000 + "|1066|9|15|1|x", "too few fields")
        self.assertIn("oversize", p.reason)

    def test_oversize_record_without_a_text_field_is_invalid(self):
        self.assert_invalid("CURIA1|7|FIN|1|2|" + "x" * 2000, "too many fields")
        self.assert_invalid("CURIA1|7|HEIR|none|" + "x" * 2000, "bytes after `none`")

    def test_overlong_line_marks_a_short_head_oversize(self):
        p = parse("CURIA1|7|VASSAL|1|1003|120|55|1523|Test Name", overlong=True)
        self.assertEqual((p.kind, p.shortened), ("record", True))
        self.assertEqual(p.text, b"Test Name")
        q = self.assert_invalid("CURIA1|7|FIN|1|2", "oversize", overlong=True)
        self.assertIn("without a text field", q.reason)


class SplitLinesTests(unittest.TestCase):
    def test_unterminated_tail_is_held_back(self):
        lines, tail = cc.split_lines(b"a\nb\nCURIA1|7|FIN|1")
        self.assertEqual([h for _n, h, _o in lines], [b"a", b"b"])
        self.assertEqual(tail, b"CURIA1|7|FIN|1")

    def test_one_carriage_return_before_the_newline_is_removed(self):
        lines, _ = cc.split_lines(b"x CURIA1|7|FIN|1|2\r\ny CURIA1|7|FIN|1|2\r\r\n")
        self.assertEqual(lines[0][1], b"x CURIA1|7|FIN|1|2")
        self.assertEqual(lines[1][1], b"y CURIA1|7|FIN|1|2\r")

    def test_overlong_line_keeps_its_first_4096_bytes(self):
        lines, _ = cc.split_lines(b"x" * 5000 + b"\n" + b"y" * 4096 + b"\n" + b"z" * 4095 + b"\r\n")
        self.assertEqual([(len(h), o) for _n, h, o in lines], [(4096, True), (4096, False), (4095, False)])

    def test_carriage_return_inside_a_record_makes_it_invalid(self):
        asm = run(["CURIA1|7|BEGIN|0|0.1.0|1066|9|15|1001|x", "CURIA1|7|FIN|312|9\r5"])
        self.assertEqual(len(asm.invalid), 1)

    def test_crlf_terminated_log(self):
        data = b"".join(prefix() + r.encode() + b"\r\n" for r in EXAMPLE)
        asm, _, _, _ = cc.analyse(data)
        self.assertEqual(kinds(asm), [("complete", "")])


class HygieneTests(unittest.TestCase):
    SAMPLE = ("Install \x15ONCLICK:CHARACTER,1 \x15TOOLTIP:CHARACTER,1 \x15L Test Ruler\x15!\x15!\x15! on the "
              "\x15ONCLICK:TITLE,2 \x15TOOLTIP:LANDED_TITLE,2 \x15L; Test Realm\x15!\x15!\x15! Throne")

    def test_markup_sample_of_the_spec(self):
        self.assertEqual(cc.hygiene(self.SAMPLE.encode()), "Install Test Ruler on the Test Realm Throne")

    def test_other_rules(self):
        self.assertEqual(cc.hygiene(b"  a \x01\t b\x7f  c  "), "a b c")
        self.assertEqual(cc.hygiene("a‮b⁦c d\u0085e".encode()), "abcde")
        self.assertEqual(cc.hygiene(b"a \x15UNKNOWN b"), "a UNKNOWN b")  # an unknown tag keeps its word
        self.assertEqual(cc.hygiene(b"bad \xff byte"), "bad � byte")

    def test_markup_sample_in_a_frame_is_cleaned_and_counted(self):
        recs = list(EXAMPLE)
        recs[4] = "CURIA1|7|VASSAL|1|1003|120|55|1523|" + self.SAMPLE
        asm = run(recs)
        self.assertEqual(kinds(asm), [("complete", "")])
        self.assertEqual(asm.hygiene_changed, 1)
        self.assertEqual(asm.hygiene_by_type["VASSAL"], 1)
        out = io.StringIO()
        cc.report(asm, 16, 0, b"", None, out)
        self.assertIn("Install Test Ruler on the Test Realm Throne", out.getvalue())
        self.assertIn("hygiene rules (link markup, control bytes): 1 (VASSAL x1)", out.getvalue())
        self.assertNotIn("\x15", out.getvalue())


class FrameTests(unittest.TestCase):
    def test_valid_frame_is_complete(self):
        asm = run(EXAMPLE)
        self.assertEqual(kinds(asm), [("complete", "")])
        f = asm.frames[0]
        self.assertEqual((f.status, f.snap, f.count, f.first_line, f.last_line), ("complete", 7, 16, 1, 16))

    def test_non_curia_lines_between_records_are_ignored(self):
        data = b""
        for i, r in enumerate(EXAMPLE):
            data += physical(r, i) + b"[" + b"00:00:01" + b"]" + b"[E]" + b"[x:1] some error text\nmore error text\n"
        asm, _, _, _ = cc.analyse(data)
        self.assertEqual(kinds(asm), [("complete", "")])

    def test_empty_sections_and_none_forms(self):
        recs = ["CURIA1|3|BEGIN|1|0.1.0|1066|1|1|5|x", "CURIA1|3|FIN|0|0", "CURIA1|3|HEIR|none", "CURIA1|3|VASSALS|0|0",
                "CURIA1|3|WARS|0", "CURIA1|3|CLAIMS|0|0|0", "CURIA1|3|FACTIONS|0|0|0", "CURIA1|3|END|8"]
        self.assertEqual(kinds(run(recs)), [("complete", "")])

    def test_unknown_records_are_counted_by_end(self):
        recs = list(EXAMPLE[:-1]) + ["CURIA1|7|PROBE|a|b", "CURIA1|7|FACTION|1|DISCONTENT|42", "CURIA1|7|END|18"]
        asm = run(recs)
        self.assertEqual(kinds(asm), [("complete", "")])
        self.assertEqual(asm.unknown_counts["PROBE"], 1)
        self.assertEqual(asm.unknown_counts["FACTION.DISCONTENT"], 1)

    def test_unknown_record_with_a_different_snap_damages(self):
        recs = list(EXAMPLE[:-1]) + ["CURIA1|8|PROBE|a", "CURIA1|7|END|17"]
        self.assertEqual(kinds(run(recs)), [("damaged", "snap mismatch")])

    def test_unknown_records_inside_a_group_do_not_break_adjacency(self):
        recs = list(EXAMPLE)
        recs.insert(12, "CURIA1|7|FACTION|1|DISCONTENT|42")
        recs[-1] = "CURIA1|7|END|17"
        self.assertEqual(kinds(run(recs)), [("complete", "")])

    def test_missing_end_then_new_begin_is_incomplete(self):
        asm = run(EXAMPLE[:5] + EXAMPLE)
        self.assertEqual(kinds(asm), [("damaged", "incomplete"), ("complete", "")])
        self.assertEqual([f.status for f in asm.frames], ["damaged", "complete"])

    def test_frame_without_end_at_eof_is_not_finished(self):
        asm = run(EXAMPLE[:5])
        self.assertEqual(asm.events, [])
        self.assertEqual(asm.frames[0].status, "open")
        self.assertIn("Not finished", cc.verdict_line(asm.frames[0]))

    def test_snap_mismatch_of_a_record(self):
        recs = list(EXAMPLE)
        recs[1] = "CURIA1|8|FIN|312|78"
        self.assertEqual(kinds(run(recs)), [("damaged", "snap mismatch")])

    def test_end_with_other_snap_is_snap_mismatch(self):
        recs = list(EXAMPLE)
        recs[-1] = "CURIA1|8|END|16"
        asm = run(recs)
        self.assertEqual(kinds(asm), [("damaged", "snap mismatch")])
        self.assertEqual(asm.state, "idle")

    def test_count_mismatch(self):
        recs = list(EXAMPLE)
        recs[-1] = "CURIA1|7|END|15"
        asm = run(recs)
        self.assertEqual(kinds(asm), [("damaged", "count mismatch")])
        self.assertIn("END says 15", asm.frames[0].detail)

    def test_invalid_record_inside_a_frame_damages_it_once(self):
        recs = list(EXAMPLE)
        recs[1] = "CURIA1|7|FIN|312"
        recs[2] = "CURIA1|7|HEIR|oops"
        asm = run(recs)
        self.assertEqual(kinds(asm), [("damaged", "invalid record")])
        self.assertEqual(len(asm.invalid), 2)
        self.assertEqual(asm.state, "idle")  # the END of the damaged frame returns to idle without a second event

    def test_invalid_begin_reports_unsupported_format_and_goes_idle(self):
        old = ["CURIA1|3|BEGIN|1066.9.15|ruler|a|b|c", "CURIA1|3|FIN|1|2", "CURIA1|3|END|3"]
        asm = run(old)
        self.assertEqual(kinds(asm), [("unsupported", "unsupported mod format")])
        self.assertEqual(asm.frames, [])
        self.assertIn("1066.9.15|ruler|a", asm.unsupported[0][1])

    def test_invalid_begin_in_an_open_frame_damages_it_first(self):
        asm = run(EXAMPLE[:4] + ["CURIA1|9|BEGIN|x"] + EXAMPLE[4:])
        self.assertEqual(kinds(asm), [("damaged", "incomplete"), ("unsupported", "unsupported mod format")])

    def test_unsupported_report_shows_control_bytes_as_question_marks_and_cuts_at_64(self):
        asm = run(["CURIA1|3|BEGIN|a\x01b|" + "c" * 100 + "|d|e"])
        self.assertIn("a?b|", asm.unsupported[0][1])
        self.assertLessEqual(len(asm.unsupported[0][1].split("BEGIN: ")[1]), 64)

    def test_snap_zero_records_never_touch_a_frame(self):
        recs = list(EXAMPLE)
        recs.insert(5, "CURIA1|0|LOAD|0.2.0")
        recs.insert(6, "CURIA1|0|BEGIN|0|x")  # invalid, snap 0: reported only
        recs[-1] = "CURIA1|7|END|16"
        asm = run(recs)
        self.assertEqual(kinds(asm), [("complete", "")])
        self.assertEqual(asm.loads, [(6, "0.2.0")])
        self.assertEqual(len(asm.invalid), 1)

    def test_records_outside_a_frame_are_ignored(self):
        asm = run(EXAMPLE[2:])  # the watcher attached mid-frame
        self.assertEqual(asm.events, [])
        self.assertEqual(asm.frames, [])

    def test_two_frames_in_a_row_and_repeated_snap(self):
        asm = run(EXAMPLE + EXAMPLE)
        self.assertEqual(kinds(asm), [("complete", ""), ("complete", "")])

    def test_513th_record_is_too_large_even_if_it_is_end(self):
        body = ["CURIA1|5|PROBE|n"] * 510
        recs = ["CURIA1|5|BEGIN|1|0.1.0|1066|1|1|5|x"] + body + ["CURIA1|5|END|512"]
        self.assertEqual(kinds(run(recs)), [("complete", "")])  # exactly 512 records
        recs = ["CURIA1|5|BEGIN|1|0.1.0|1066|1|1|5|x"] + body + ["CURIA1|5|PROBE|n", "CURIA1|5|END|513"]
        asm = run(recs)
        self.assertEqual(kinds(asm), [("damaged", "too large")])
        self.assertEqual(asm.frames[0].count, 512)

    def test_bytes_above_128_kib_are_too_large(self):
        big = "CURIA1|5|PROBE|" + "x" * 3000
        recs = ["CURIA1|5|BEGIN|1|0.1.0|1066|1|1|5|x"] + [big] * 44 + ["CURIA1|5|END|46"]
        self.assertEqual(kinds(run(recs)), [("damaged", "too large")])
        recs = ["CURIA1|5|BEGIN|1|0.1.0|1066|1|1|5|x"] + [big] * 40 + ["CURIA1|5|END|42"]
        self.assertEqual(kinds(run(recs)), [("complete", "")])

    def test_begin_after_a_too_large_frame_starts_with_a_fresh_count(self):
        body = ["CURIA1|5|PROBE|n"] * 600
        asm = run(["CURIA1|5|BEGIN|1|0.1.0|1066|1|1|5|x"] + body + EXAMPLE)
        self.assertEqual(kinds(asm), [("damaged", "too large"), ("complete", "")])

    def test_shortened_text_keeps_the_frame_complete_and_flags_it(self):
        recs = list(EXAMPLE)
        recs[4] = "CURIA1|7|VASSAL|1|1003|120|55|1523|" + "N" * 3000
        asm = run(recs)
        self.assertEqual(kinds(asm), [("complete", "text shortened")])
        self.assertTrue(asm.frames[0].shortened)
        self.assertIn("text shortened", cc.verdict_line(asm.frames[0]))

    def test_shortened_ruler_name_flags_the_frame(self):
        recs = list(EXAMPLE)
        recs[0] = "CURIA1|7|BEGIN|0|0.1.0|1066|9|15|1001|" + "R" * 2000
        self.assertEqual(kinds(run(recs)), [("complete", "text shortened")])

    def test_oversize_record_without_text_is_an_invalid_record(self):
        recs = list(EXAMPLE)
        recs[1] = "CURIA1|7|FIN|312|78|" + "x" * 2000
        self.assertEqual(kinds(run(recs)), [("damaged", "invalid record")])


class SectionRuleTests(unittest.TestCase):
    def frame(self, *middle, snap=7):
        recs = ["CURIA1|%d|BEGIN|0|0.1.0|1066|9|15|1001|Test Ruler One" % snap]
        recs += ["CURIA1|%d|%s" % (snap, m) for m in middle]
        recs.append("CURIA1|%d|END|%d" % (snap, len(recs) + 1))
        return run(recs)

    def test_a_skipped_section_is_not_an_error(self):
        self.assertEqual(kinds(self.frame("FIN|1|2")), [("complete", "")])

    def test_sections_out_of_order(self):
        self.assertEqual(kinds(self.frame("WARS|0", "FIN|1|2")), [("damaged", "order")])

    def test_a_section_twice(self):
        self.assertEqual(kinds(self.frame("FIN|1|2", "FIN|1|2")), [("damaged", "order")])

    def test_item_outside_its_section(self):
        self.assertEqual(kinds(self.frame("FIN|1|2", "VASSAL|1|1|1|1|1|x")), [("damaged", "order")])
        self.assertEqual(kinds(self.frame("VASSALS|0|0", "WARS|0", "VASSAL|1|1|1|1|1|x")), [("damaged", "order")])

    def test_vassals_listed_must_equal_the_records(self):
        a = self.frame("VASSALS|2|2", "VASSAL|1|1|1|1|1|x")
        self.assertEqual(kinds(a), [("damaged", "count mismatch")])

    def test_vassal_rank_runs_in_order(self):
        a = self.frame("VASSALS|2|2", "VASSAL|2|1|1|1|1|x", "VASSAL|1|2|1|1|1|y")
        self.assertEqual(kinds(a), [("damaged", "order")])

    def test_vassals_listed_at_most_total(self):
        a = self.frame("VASSALS|1|2", "VASSAL|1|1|1|1|1|x", "VASSAL|2|2|1|1|1|y")
        self.assertEqual(kinds(a), [("damaged", "count mismatch")])

    def test_wars_group_and_total(self):
        good = ["WARS|2", "WAR|1|NAME|First War", "WAR|1|ATTACKER|1001|Test Ruler One",
                "WAR|1|DEFENDER|1002|Test Enemy", "WAR|2|NAME|Second War", "WAR|2|ATTACKER|1003|Test Ally",
                "WAR|2|DEFENDER|1001|Test Ruler One"]
        self.assertEqual(kinds(self.frame(*good)), [("complete", "")])
        self.assertEqual(kinds(self.frame("WARS|3", *good[1:])), [("damaged", "count mismatch")])
        swapped = good[:2] + [good[3], good[2]] + good[4:]
        self.assertEqual(kinds(self.frame(*swapped)), [("damaged", "order")])
        other_n = list(good)
        other_n[3] = "WAR|2|DEFENDER|1002|Test Enemy"
        self.assertEqual(kinds(self.frame(*other_n)), [("damaged", "order")])
        wrong_n = list(good)
        wrong_n[1:4] = ["WAR|2|NAME|First War", "WAR|2|ATTACKER|1001|x", "WAR|2|DEFENDER|1002|y"]
        self.assertEqual(kinds(self.frame(*wrong_n)), [("damaged", "order")])
        self.assertEqual(kinds(self.frame(*good[:3])), [("damaged", "order")])  # a group of two records

    def test_war_sides_in_the_table(self):
        good = ["WARS|2", "WAR|1|NAME|First War", "WAR|1|ATTACKER|1001|Test Ruler One",
                "WAR|1|DEFENDER|1002|Test Enemy", "WAR|2|NAME|Second War", "WAR|2|ATTACKER|1003|Test Ally",
                "WAR|2|DEFENDER|1004|Test Other"]
        out = io.StringIO()
        cc.report(self.frame(*good), 0, 0, b"", None, out)
        text = out.getvalue()
        self.assertIn("the ruler is the ATTACKER", text)
        self.assertIn("an ally", text)

    def test_claims_rules(self):
        self.assertEqual(kinds(self.frame("CLAIMS|2|0|1", "CLAIM|1|strong|3|Test Duchy")), [("complete", "")])
        self.assertEqual(kinds(self.frame("CLAIMS|2|0|2", "CLAIM|1|strong|3|a")), [("damaged", "count mismatch")])
        self.assertEqual(kinds(self.frame("CLAIMS|1|0|2", "CLAIM|1|strong|3|a", "CLAIM|2|weak|3|b")),
                         [("damaged", "count mismatch")])
        self.assertEqual(kinds(self.frame("CLAIMS|2|0|2", "CLAIM|2|strong|3|a", "CLAIM|1|weak|3|b")),
                         [("damaged", "order")])

    def faction_group(self, n=1, role="against", ftype=1):
        return ["FACTION|%d|INFO|%s|%d|10|100|2" % (n, role, ftype), "FACTION|%d|LEADER|none" % n,
                "FACTION|%d|CLAIMANT|none" % n, "FACTION|%d|TITLE|none" % n, "FACTION|%d|TARGET|none" % n]

    def test_faction_rules(self):
        one = self.faction_group()
        self.assertEqual(kinds(self.frame("FACTIONS|1|0|1", *one)), [("complete", "")])
        self.assertEqual(kinds(self.frame("FACTIONS|1|0|2", *one)), [("damaged", "count mismatch")])
        self.assertEqual(kinds(self.frame("FACTIONS|0|0|1", *one)), [("damaged", "count mismatch")])  # against > header
        self.assertEqual(kinds(self.frame("FACTIONS|3|0|1", *one)), [("complete", "")])  # capped listing is fine
        mem = self.faction_group(role="member")
        self.assertEqual(kinds(self.frame("FACTIONS|0|1|1", *mem)), [("complete", "")])
        self.assertEqual(kinds(self.frame("FACTIONS|0|0|1", *mem)), [("damaged", "count mismatch")])
        two = one + self.faction_group(2, "member")
        self.assertEqual(kinds(self.frame("FACTIONS|1|1|2", *two)), [("complete", "")])
        reordered = [one[0], one[2], one[1]] + one[3:]
        self.assertEqual(kinds(self.frame("FACTIONS|1|0|1", *reordered)), [("damaged", "order")])
        missing = one[:3] + one[4:]
        self.assertEqual(kinds(self.frame("FACTIONS|1|0|1", *missing)), [("damaged", "order")])
        bad_n = one[:4] + ["FACTION|2|TARGET|none"]
        self.assertEqual(kinds(self.frame("FACTIONS|1|0|1", *bad_n)), [("damaged", "order")])

    def test_reason_precedence_snap_then_count_then_order(self):
        # an order problem (sections swapped) and a wrong END count together: count mismatch wins
        recs = ["CURIA1|7|BEGIN|0|0.1.0|1066|9|15|1001|x", "CURIA1|7|WARS|0", "CURIA1|7|FIN|1|2", "CURIA1|7|END|9"]
        self.assertEqual(kinds(run(recs)), [("damaged", "count mismatch")])

    def test_caps_are_not_the_parsers_business(self):
        items = ["VASSAL|%d|%d|1|1|1|x" % (i, i) for i in range(1, 13)]
        self.assertEqual(kinds(self.frame("VASSALS|15|12", *items)), [("complete", "")])


class OutputTests(unittest.TestCase):
    def render(self, records, **kw):
        asm = run(records)
        out = io.StringIO()
        code = cc.report(asm, len(records), 0, b"", kw.get("last"), out)
        return code, out.getvalue()

    def test_income_is_shown_as_tenths_and_gold(self):
        self.assertEqual([cc.tenths(v) for v in (78, 0, -5, -78, 10, 5, -10)],
                         ["7.8", "0.0", "-0.5", "-7.8", "1.0", "0.5", "-1.0"])
        code, text = self.render(EXAMPLE)
        self.assertIn("78 tenths = 7.8 gold per month", text)
        self.assertEqual(code, 0)

    def test_frame_table_has_the_required_content(self):
        _code, text = self.render(EXAMPLE)
        for needle in ("VERDICT: Complete", "Test Ruler One (id 1001)", "1066-09-15", "September", "unpressed",
                       "4 kingdom", "type 3 claimant", "power 33, threshold 100, members 2", "leader:",
                       "claimant: Test Claimant Five (id 1005)", "title:", "target:", "COMPARE WITH THE GAME UI",
                       "SUMMARY", "VASSAL.opinion  -12", "sign byte 0x2d", "frames: 1 (Complete 1, Damaged 0"):
            self.assertIn(needle, text)

    def test_no_engine_prefix_is_ever_printed(self):
        recs = list(EXAMPLE)
        recs[1] = "CURIA1|7|FIN|312"
        _code, text = self.render(recs)
        self.assertNotIn("file:", text)
        self.assertNotIn("test_effect", text)
        self.assertNotIn("jomini", text)
        self.assertIn("record: CURIA1|7|FIN|312", text)

    def test_damaged_frame_verdict_and_summary(self):
        recs = list(EXAMPLE)
        recs[-1] = "CURIA1|7|END|3"
        code, text = self.render(recs)
        self.assertEqual(code, 1)
        self.assertIn("VERDICT: Damaged: count mismatch", text)
        self.assertIn("damaged: snap 7, lines 1 to 16: count mismatch", text)

    def test_grammar_failures_are_listed_with_line_and_reason(self):
        recs = list(EXAMPLE)
        recs[2] = "CURIA1|7|HEIR|1002|84|1|0"
        _code, text = self.render(recs)
        self.assertIn("records that failed the grammar: 1", text)
        self.assertIn("line 3: HEIR: text delimiter missing", text)

    def test_text_shortened_is_counted(self):
        recs = list(EXAMPLE)
        recs[4] = "CURIA1|7|VASSAL|1|1003|120|55|1523|" + "N" * 3000
        _code, text = self.render(recs)
        self.assertIn("frames with text shortened: 1", text)

    def test_negative_forms_and_none_seen(self):
        recs = list(EXAMPLE)
        recs[5] = "CURIA1|7|VASSAL|2|1004|40|5|310|Test Vassal Four"
        _code, text = self.render(recs)
        self.assertIn("negative integers seen: none in this log", text)
        recs[1] = "CURIA1|7|FIN|-30|-5"
        _code, text = self.render(recs)
        self.assertIn("FIN.gold  -30", text)
        self.assertIn("FIN.income  -5", text)
        self.assertIn("-0.5 gold per month", text)

    def test_load_line_is_listed(self):
        _code, text = self.render(["CURIA1|0|LOAD|0.2.0"] + EXAMPLE)
        self.assertIn("LOAD records (snap 0): line 1 version 0.2.0", text)

    def test_ruler_side_for_allies_and_claimant_note_for_other_types(self):
        recs = list(EXAMPLE)
        recs[10] = "CURIA1|7|FACTION|1|INFO|against|5|33|100|2"
        _code, text = self.render(recs)
        self.assertIn("[type is not claimant: the app ignores this]", text)

    def test_last_option_limits_tables_not_the_summary(self):
        _code, text = self.render(EXAMPLE + EXAMPLE, last=1)
        self.assertEqual(text.count("VERDICT:"), 1)
        self.assertIn("1 earlier frame(s) not shown", text)
        self.assertIn("frames: 2 (Complete 2", text)

    def test_unterminated_tail_is_mentioned_and_ignored(self):
        data = b"".join(physical(r, i) for i, r in enumerate(EXAMPLE)) + prefix() + b"CURIA1|8|BEGIN|0|0.1.0|10"
        asm, total, over, tail = cc.analyse(data)
        out = io.StringIO()
        cc.report(asm, total, over, tail, None, out)
        self.assertIn("unterminated last line ignored", out.getvalue())
        self.assertEqual(len(asm.frames), 1)

    def test_no_curia_records_gives_a_message_and_status_one(self):
        code, text = self.render([])
        self.assertEqual(code, 1)
        self.assertIn("no Curia frame found", text)


class MainTests(unittest.TestCase):
    def test_main_reads_a_file(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "debug.log")
            with open(path, "wb") as fh:
                fh.write(b"unrelated line\n" + b"".join(physical(r, i) for i, r in enumerate(EXAMPLE)))
            out = io.StringIO()
            self.assertEqual(cc.main([path], out), 0)
            self.assertIn("VERDICT: Complete", out.getvalue())

    def test_non_ascii_names_survive_an_ascii_stdout(self):
        recs = [r.replace("Test Ruler One", "Test R\u00f6ler \u00d6ne") for r in EXAMPLE]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "debug.log")
            with open(path, "wb") as fh:
                fh.write(b"".join(physical(r, i) for i, r in enumerate(recs)))
            env = dict(os.environ, PYTHONIOENCODING="ascii")
            script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "curia1_check.py")
            res = subprocess.run([sys.executable, script, path], env=env, capture_output=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn(b"VERDICT: Complete", res.stdout)

    def test_missing_file_is_status_two(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(cc.main([os.path.join(tempfile.gettempdir(), "no-such-curia-log.log")], io.StringIO()), 2)
        self.assertIn("cannot read", err.getvalue())


if __name__ == "__main__":
    unittest.main()
