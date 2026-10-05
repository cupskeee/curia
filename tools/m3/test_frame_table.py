# SPDX-License-Identifier: MIT
"""Tests for frame_table.py (synthetic watcher records only)."""
import json
import os
import tempfile
import unittest

import frame_table


def frame(snap, kinds, label, lines, payload, raw, complete=True):
    return json.dumps({"type": "frame", "snap_id": snap, "kinds": kinds, "label": label, "lines": lines,
                       "declared_count": lines, "payload_bytes": payload, "raw_bytes_with_prefix": raw,
                       "longest_line_bytes": 90, "span_s": 0.0, "complete": complete})


class FrameTableTests(unittest.TestCase):
    def setUp(self):
        self.lines = [
            json.dumps({"type": "sample"}),
            frame(1, ["BEGIN", "FIN", "END"], "paused", 3, 100, 400),
            frame(2, ["BEGIN", "FIN", "PROBE", "END"], "paused", 4, 200, 800),
            frame(3, ["BEGIN", "FIN", "END"], "menu", 3, 120, 420),
            frame(4, ["BEGIN"], "menu", 1, 30, 140, complete=False),
            "",
        ]

    def test_buttons_are_told_apart_by_the_probe_kind(self):
        frames = frame_table.load_frames(self.lines)
        self.assertEqual([f["button"] for f in frames], ["advisor", "probes", "advisor", "advisor"])

    def test_report_counts_only_complete_frames(self):
        text = frame_table.format_report(frame_table.load_frames(self.lines))
        self.assertIn("paused advisor 1", text)
        self.assertIn("paused probes 1", text)
        self.assertIn("menu advisor 1", text)
        self.assertIn("advisor lines: 3 / 3.0 / 3 (n=2)", text)
        self.assertIn("advisor text bytes: 100 / 110.0 / 120 (n=2)", text)
        self.assertIn("probes file bytes: 800 / 800 / 800 (n=1)", text)

    def test_unreadable_lines_are_skipped_and_counted(self):
        skipped = [0]
        frames = frame_table.load_frames(self.lines + ['{"type": "frame", "snap_id"', "[]", "5"], skipped)
        self.assertEqual(len(frames), 4)
        self.assertEqual(skipped[0], 3)

    def test_long_labels_are_cut(self):
        long_label = "x" * 200
        text = frame_table.format_report(frame_table.load_frames([frame(9, ["END"], long_label, 1, 5, 9)]))
        self.assertNotIn("x" * 41, text)

    def test_empty_input_gives_a_header_and_no_statistics(self):
        text = frame_table.format_report(frame_table.load_frames([]))
        self.assertTrue(text.startswith("snap | button"))
        self.assertNotIn("(n=", text)

    def test_newest_file_is_picked(self):
        with tempfile.TemporaryDirectory() as tmp:
            old = os.path.join(tmp, "watch-1.jsonl")
            new = os.path.join(tmp, "watch-2.jsonl")
            for path, stamp in ((old, 1000), (new, 2000)):
                open(path, "w").close()
                os.utime(path, (stamp, stamp))
            self.assertEqual(frame_table.newest_watch_file(tmp), new)
            self.assertIsNone(frame_table.newest_watch_file(os.path.join(tmp, "missing")))


if __name__ == "__main__":
    unittest.main()
