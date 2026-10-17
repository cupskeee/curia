# SPDX-License-Identifier: MIT
"""Unit tests for tools/m4/quality_table.py (synthetic data only).

Run: python3 -m unittest discover -s tools/m4 -v
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import tempfile
import unittest

import quality_table as qt


def jsonl(*events: object) -> str:
    return "\n".join(json.dumps(e) for e in events) + "\n"


READY = {"event": "ready", "model": "mock-model-1", "snapshot": 3}
DONE = {"event": "done", "generation": 1, "text": "Check your heir.", "first_token_ms": 812,
        "total_ms": 3100, "usage": {"input": 4000, "output": 120, "cache_read": 3500, "cache_write": 0},
        "stop_reason": "end_turn", "max_tokens": False}


class RowTests(unittest.TestCase):
    def test_a_complete_answer(self):
        rows = qt.rows_from_events([READY, {"event": "ask", "generation": 1, "text": "is my heir safe?"},
                                    {"event": "token", "text": "x"}, DONE])
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual((row["model"], row["question"], row["first_token_ms"], row["total_ms"]),
                         ("mock-model-1", "is my heir safe?", 812, 3100))
        self.assertEqual((row["input"], row["output"], row["cache_read"], row["cache_write"]),
                         (4000, 120, 3500, 0))
        self.assertEqual(row["result"], "ok")
        self.assertEqual(row["snap"], 3)
        self.assertEqual(row["chars"], len("Check your heir."))
        self.assertEqual(row["words"], 3)
        self.assertEqual(row["setting"], "default")  # an older run: no settings in the events

    def test_other_endings(self):
        events = [READY,
                  {"event": "ask", "text": "q1"}, {"event": "cancelled", "total_ms": 900},
                  {"event": "ask", "text": "q2"}, {"event": "error", "code": "Overloaded", "total_ms": 40,
                                                   "partial_len": 17},
                  {"event": "ask", "text": "q3"}, dict(DONE, max_tokens=True),
                  {"event": "ask", "text": "q4"}]  # the run ended before an answer
        rows = qt.rows_from_events(events)
        self.assertEqual([r["result"] for r in rows],
                         ["stopped", "error Overloaded", "cut off (max_tokens)", "no result"])
        self.assertEqual(rows[1]["chars"], 17)
        self.assertIsNone(rows[3]["total_ms"])

    def test_missing_fields_are_tolerated(self):
        rows = qt.rows_from_events([{"event": "ask"}, {"event": "done"}, "not a dict", {"event": "ready"}])
        self.assertEqual(rows[0]["question"], "-")
        self.assertIsNone(rows[0]["first_token_ms"])
        cells = qt.row_cells(rows[0])
        self.assertEqual(cells[0], "-")  # no file name: the label is the model, here "-"
        self.assertEqual(cells[2], "default")  # no setting
        self.assertEqual(cells[4], "-")  # no snapshot number
        self.assertEqual(cells[5], "-")  # no first-token time
        self.assertEqual(cells[9], "-")  # no words
        self.assertEqual(len(cells), len(qt.COLUMNS))

    def test_several_questions_and_models_in_order(self):
        a = [READY, {"event": "ask", "text": "one"}, DONE, {"event": "ask", "text": "two"}, dict(DONE, total_ms=5000)]
        b = [dict(READY, model="mock-model-2"), {"event": "ask", "text": "one"}, dict(DONE, first_token_ms=100)]
        rows = qt.rows_from_events(a) + qt.rows_from_events(b)
        summary = qt.summary_rows(rows)
        # Without a file name the label is the model.
        self.assertEqual([s[0] for s in summary], ["mock-model-1", "mock-model-2"])
        self.assertEqual(summary[0][1:], ("default", 2, 812, 4050, 3, 0))
        self.assertEqual(summary[1][1:], ("default", 1, 100, 3100, 3, 0))


class SettingTests(unittest.TestCase):
    def test_the_setting_comes_from_the_done_event_then_the_ready_event(self):
        ready = dict(READY, effort="low", thinking="default")
        done = dict(DONE, effort="low", thinking="between_tools", text="one two  three\nfour")
        rows = qt.rows_from_events([ready, {"event": "ask", "text": "q1"}, done,
                                    {"event": "ask", "text": "q2"}, {"event": "error", "code": "Other"}])
        self.assertEqual(rows[0]["setting"], "effort low, thinking between_tools")
        self.assertEqual(rows[0]["words"], 4)
        self.assertEqual(rows[1]["setting"], "effort low")  # no done event: the ready event's
        self.assertIsNone(rows[1]["words"])
        self.assertEqual(qt.setting_of({"effort": "default", "thinking": "disabled"}), "thinking disabled")
        self.assertEqual(qt.setting_of({}), "default")
        self.assertEqual(qt.setting_of({"effort": 3, "thinking": None}), "default")

    def test_labels_come_from_the_file_names(self):
        self.assertEqual(qt.label_of("sonnet2-low-q3.jsonl"), "sonnet2-low")
        self.assertEqual(qt.label_of("/x/y/haiku-q12.jsonl"), "haiku")
        self.assertEqual(qt.label_of("run.jsonl"), "run")
        self.assertEqual(qt.label_of(""), "")

    def test_medians_per_label_and_setting(self):
        def run(label: str, effort: str, totals: list) -> list:
            rows = []
            for n, total in enumerate(totals, 1):
                rows += qt.rows_from_events(
                    [dict(READY, effort=effort), {"event": "ask", "text": "q%d" % n},
                     dict(DONE, total_ms=total, effort=effort, text="w " * (10 * n))],
                    "%s-q%d.jsonl" % (label, n))
            return rows
        rows = run("sonnet2", "default", [3000, 5000, 4000]) + run("sonnet2-low", "low", [1000, 2000])
        summary = qt.summary_rows(rows)
        self.assertEqual(summary, [("sonnet2", "default", 3, 812, 4000, 20, 0),
                                   ("sonnet2-low", "effort low", 2, 812, 1500, 15, 0)])
        text = qt.markdown(rows)
        self.assertIn("| Run | Setting | Questions | Median first token (ms) | Median total (ms) | Median words "
                      "| Not ok |", text)
        self.assertIn("| sonnet2-low | effort low | 2 | 812 | 1500 | 15 | 0 |", text)


class OutputTests(unittest.TestCase):
    def test_markdown_has_a_header_rows_and_the_summary_and_escapes_pipes(self):
        rows = qt.rows_from_events([READY, {"event": "ask", "text": "a | b"}, DONE])
        text = qt.markdown(rows)
        lines = text.splitlines()
        self.assertTrue(lines[0].startswith("| Run | Model | Setting | Question | Snap | First token (ms)"))
        self.assertIn("| mock-model-1 | mock-model-1 | default | a / b | 3 | 812 | 3100 | 4000 | 120 | 3 | 3500 | 0 "
                      "| ok |  |  |", lines)
        self.assertIn("| Run | Setting | Questions | Median first token (ms) | Median total (ms) | Median words "
                      "| Not ok |", lines)
        self.assertNotIn("Check your heir", text)  # answers are never printed

    def test_csv_quotes(self):
        rows = qt.rows_from_events([READY, {"event": "ask", "text": 'say "hi", please'}, DONE])
        text = qt.csv_text(rows)
        self.assertIn('"say ""hi"", please"', text)

    def test_files_and_folders_with_junk_lines(self):
        with tempfile.TemporaryDirectory() as folder:
            with open(os.path.join(folder, "a.jsonl"), "w", encoding="utf-8") as handle:
                handle.write("garbage\n" + jsonl(READY, {"event": "ask", "text": "q"}, DONE) + "{broken\n")
            with open(os.path.join(folder, "ignored.bin"), "wb") as handle:
                handle.write(b"\xff\xfe")
            with open(os.path.join(folder, "b.jsonl"), "wb") as handle:
                handle.write(b"\xff\xfe\n" + jsonl(READY, {"event": "ask", "text": "r"}, DONE).encode())
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(qt.main([folder]), 0)
            self.assertEqual(out.getvalue().count("| mock-model-1 |"), 2)  # the two rows
            self.assertIn("| a | mock-model-1 | default | q |", out.getvalue())  # rows carry the file label
            self.assertIn("| a | default | 1 |", out.getvalue())  # the summary, per file label
            self.assertIn("| b | default | 1 |", out.getvalue())
            self.assertEqual(qt.read_events(os.path.join(folder, "missing.jsonl")), [])

    def test_no_questions_is_exit_1(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "empty.jsonl")
            open(path, "w").close()
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(qt.main([path]), 1)
            self.assertIn("No questions", err.getvalue())


if __name__ == "__main__":
    unittest.main()
