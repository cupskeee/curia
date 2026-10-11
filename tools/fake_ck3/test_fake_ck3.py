# SPDX-License-Identifier: MIT
"""Unit tests for tools/fake_ck3/fake_ck3.py. Standard library only; synthetic data and temp folders.

Run: python3 -m unittest discover -s tools/fake_ck3 -v
No test waits on the wall clock: the sleep function of the fake is replaced where delays matter.
"""
from __future__ import annotations

import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
import unittest
from typing import List

import fake_ck3 as fk

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "..", "..", "tests", "fixtures", "curia1")
ENGINE_LINE = re.compile(rb"\[[0-9]{2}:[0-9]{2}:[0-9]{2}\]\[[DIEW]\]\[")


def read_lines(fake: fk.FakeCk3) -> List[bytes]:
    with open(fake.log_path, "rb") as handle:
        data = handle.read()
    return data.split(b"\n")[:-1] if data else []


def records(lines: List[bytes]) -> List[bytes]:
    """The part of every line from the marker on (what the app calls the record)."""
    return [line[line.index(fk.MARKER):] for line in lines if fk.MARKER in line]


class PrefixTests(unittest.TestCase):
    def test_length_is_119_to_131_and_deterministic(self):
        lengths = {len(fk.engine_prefix(v)) for v in range(60)}
        self.assertEqual(lengths, set(range(119, 132)))
        self.assertEqual(fk.engine_prefix(5), fk.engine_prefix(5))

    def test_never_contains_the_marker_or_an_engine_line_pattern(self):
        for variant in range(30):
            prefix = fk.engine_prefix(variant)
            self.assertNotIn(fk.MARKER, prefix)
            self.assertIsNone(ENGINE_LINE.search(prefix))
            prefix.decode("ascii")

    def test_this_tool_embeds_no_engine_line(self):
        for name in ("fake_ck3.py", "test_fake_ck3.py", "e2e_test.py"):
            path = os.path.join(HERE, name)
            if not os.path.exists(path):
                continue
            with open(path, "rb") as handle:
                text = handle.read()
            self.assertIsNone(ENGINE_LINE.search(text), name)


class LayoutTests(unittest.TestCase):
    def test_plain_layout(self):
        with fk.FakeCk3() as fake:
            fake.launch()
            self.assertTrue(fake.log_path.endswith(os.path.join("logs", "debug.log")))
            self.assertTrue(os.path.isfile(fake.log_path))
            self.assertEqual(fake.size(), 0)

    def test_wine_layout_is_nested_and_created(self):
        with fk.FakeCk3(layout="wine") as fake:
            fake.launch()
            self.assertIn(os.path.join("drive_c", "users"), fake.log_path)
            self.assertIn("Crusader Kings III", fake.log_path)
            self.assertTrue(os.path.isfile(fake.log_path))

    def test_temporary_root_is_removed_but_a_given_root_is_kept(self):
        fake = fk.FakeCk3()
        fake.launch()
        root = fake.root
        fake.close()
        self.assertFalse(os.path.exists(root))
        with tempfile.TemporaryDirectory() as given:
            fake = fk.FakeCk3(root=given)
            fake.launch()
            fake.close()
            self.assertTrue(os.path.exists(fake.log_path))

    def test_unknown_layout_is_refused(self):
        with self.assertRaises(ValueError):
            fk.FakeCk3(layout="mac")


class FrameTests(unittest.TestCase):
    def test_example_frame_is_the_spec_frame_with_the_right_end_count(self):
        frame = fk.example_frame_records(7)
        self.assertEqual(len(frame), 16)
        self.assertEqual(frame[0], b"CURIA1|7|BEGIN|0|0.1.0|1066|9|15|1001|Test Ruler One")
        self.assertEqual(frame[-1], b"CURIA1|7|END|16")

    def test_every_line_has_a_prefix_and_the_records_survive(self):
        with fk.FakeCk3(snap_start=7) as fake:
            fake.launch()
            self.assertEqual(fake.append_frame(), 7)
            lines = read_lines(fake)
            self.assertEqual(len(lines), 16)
            for line in lines:
                self.assertGreaterEqual(line.index(fk.MARKER), 119)
                self.assertLessEqual(line.index(fk.MARKER), 131)
            self.assertEqual(records(lines), fk.example_frame_records(7))

    def test_snaps_count_up(self):
        with fk.FakeCk3(snap_start=3) as fake:
            fake.launch()
            snaps = [fake.append_frame() for _ in range(3)]
            self.assertEqual(snaps, [3, 4, 5])
            self.assertEqual(len(read_lines(fake)), 48)

    def test_per_line_and_burst_write_patterns(self):
        with fk.FakeCk3(keep_chunks=True) as fake:
            fake.launch()
            fake.append_frame()
            self.assertEqual(len(fake.chunks), 16)
        with fk.FakeCk3(keep_chunks=True, burst=True) as fake:
            fake.launch()
            fake.append_frame()
            self.assertEqual(len(fake.chunks), 1)
            self.assertEqual(len(read_lines(fake)), 16)

    def test_line_buffer_splits_every_line_and_the_file_is_the_same(self):
        pauses: List[float] = []
        with fk.FakeCk3(keep_chunks=True, line_buffer=True, sleep=pauses.append) as fake:
            fake.launch()
            fake.append_frame()
            self.assertEqual(len(fake.chunks), 32)
            self.assertEqual(len(pauses), 16)
            self.assertEqual(records(read_lines(fake)), fk.example_frame_records(1))

    def test_flush_delay_is_applied_to_each_write(self):
        pauses: List[float] = []
        with fk.FakeCk3(flush_delay=0.25, sleep=pauses.append) as fake:
            fake.launch()
            fake.append_frame()
            self.assertEqual(pauses, [0.25] * 16)

    def test_partial_frame_stops_before_end_and_can_be_finished(self):
        with fk.FakeCk3() as fake:
            fake.launch()
            fake.append_frame(partial=True)
            self.assertEqual(len(read_lines(fake)), 15)
            self.assertNotIn(b"|END|", b"\n".join(read_lines(fake)))
            fake.finish_frame()
            self.assertEqual(records(read_lines(fake)), fk.example_frame_records(1))
            fake.finish_frame()  # nothing left: no second END
            self.assertEqual(len(read_lines(fake)), 16)

    def test_interleave_adds_noise_and_multiline_entries_inside_the_frame(self):
        with fk.FakeCk3(interleave=True) as fake:
            fake.launch()
            fake.append_frame()
            lines = read_lines(fake)
            self.assertGreater(len(lines), 16)
            self.assertEqual(records(lines), fk.example_frame_records(1))
            # The first and the last line are still the frame's own.
            self.assertIn(b"|BEGIN|", lines[0])
            self.assertIn(b"|END|", lines[-1])
            # Some lines are continuation lines of an error entry: no prefix, no marker.
            self.assertTrue(any(line.startswith(b"    synthetic detail") for line in lines))

    def test_die_after_bytes_cuts_the_log_exactly_and_stays_dead(self):
        with fk.FakeCk3(die_after_bytes=500) as fake:
            fake.launch()
            fake.append_frame()
            self.assertEqual(fake.size(), 500)
            self.assertTrue(fake.dead)
            fake.append_frame()
            fake.append_noise(3)
            self.assertEqual(fake.size(), 500)

    def test_noise_lines_carry_the_prefix_and_no_marker(self):
        with fk.FakeCk3() as fake:
            fake.launch()
            fake.append_noise(4)
            lines = read_lines(fake)
            self.assertEqual(len(lines), 4)
            for line in lines:
                self.assertNotIn(fk.MARKER, line)
                self.assertTrue(line.startswith(b"synthetic-time"))


class TruncationTests(unittest.TestCase):
    def test_truncate_at_launch_empties_the_same_file(self):
        with fk.FakeCk3() as fake:
            fake.launch()
            fake.append_frame()
            before = os.stat(fake.log_path)
            self.assertGreater(before.st_size, 0)
            fake.launch(truncate=True)
            after = os.stat(fake.log_path)
            self.assertEqual(after.st_size, 0)
            if os.name == "posix":
                self.assertEqual(before.st_ino, after.st_ino)
            # The game keeps writing through its open handle.
            fake.append_frame()
            self.assertEqual(len(read_lines(fake)), 16)
            self.assertIn(b"|BEGIN|", read_lines(fake)[0])


class FixtureTests(unittest.TestCase):
    def fixture(self, name: str) -> str:
        return os.path.join(FIXTURES, name + ".log")

    def test_parse_rejects_unknown_lines(self):
        with self.assertRaises(ValueError):
            fk.parse_fixture(b"CURIA1|1|END|1\nwhat is this\n")

    def test_directives(self):
        data = b"# comment\n\nCURIA1|1|BEGIN|x\n!noise plain noise\n!raw CURIA1|raw marker\n!noise\n!truncate\nCURIA1|2|END|1\n"
        actions = fk.parse_fixture(data)
        self.assertEqual([a.truncate for a in actions], [False, True, False])
        first = actions[0].lines
        self.assertEqual(len(first), 4)
        self.assertTrue(first[0].endswith(b"CURIA1|1|BEGIN|x") and first[0] != b"CURIA1|1|BEGIN|x")
        self.assertEqual(first[1], b"plain noise")
        self.assertEqual(first[2], b"CURIA1|raw marker")  # no prefix: the marker is at offset 0
        self.assertEqual(first[3], b"")

    def test_every_fixture_parses(self):
        names = [n for n in os.listdir(FIXTURES) if n.endswith(".log")]
        self.assertGreater(len(names), 20)
        for name in names:
            with open(os.path.join(FIXTURES, name), "rb") as handle:
                fk.parse_fixture(handle.read())

    def test_example_fixture_plays_as_the_builtin_frame(self):
        with fk.FakeCk3() as fake:
            fake.launch()
            fake.play_fixture(self.fixture("example_frame"))
            self.assertEqual(records(read_lines(fake)), fk.example_frame_records(7))

    def test_truncate_directive_empties_the_log_and_calls_the_hook(self):
        calls: List[tuple] = []
        with fk.FakeCk3() as fake:
            fake.launch()
            fake.play_fixture(self.fixture("damaged_truncated"),
                              between=lambda kind: calls.append((kind, fake.size())))
            # The fixture truncates twice; the hook runs before each, while the log is not empty,
            # and after each, when it is.
            self.assertEqual([kind for kind, _ in calls],
                             ["truncate", "truncated", "truncate", "truncated"])
            self.assertTrue(calls[0][1] > 0 and calls[2][1] > 0)
            self.assertEqual((calls[1][1], calls[3][1]), (0, 0))
            # What is left is the last step: one frame of two lines.
            self.assertEqual(len(read_lines(fake)), 2)


class CommandLineTests(unittest.TestCase):
    def run_tool(self, *args: str) -> str:
        done = subprocess.run([sys.executable, os.path.join(HERE, "fake_ck3.py"), *args],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60, check=True)
        return done.stdout.decode()

    def test_writes_frames_with_repeat_and_snap_start(self):
        with tempfile.TemporaryDirectory() as root:
            out = self.run_tool("--root", root, "--repeat", "3", "--snap-start", "5")
            log = re.search(r"^LOG (.*)$", out, re.M).group(1)
            self.assertTrue(log.startswith(root))
            with open(log, "rb") as handle:
                data = handle.read()
            snaps = [int(m.group(1)) for m in re.finditer(rb"CURIA1\|(\d+)\|BEGIN", data)]
            self.assertEqual(snaps, [5, 6, 7])
            self.assertIn("DONE %d" % len(data), out)

    def test_prior_frames_then_truncate_at_launch(self):
        with tempfile.TemporaryDirectory() as root:
            out = self.run_tool("--root", root, "--prior-frames", "2", "--truncate-at-launch",
                                "--repeat", "1", "--snap-start", "9")
            log = re.search(r"^LOG (.*)$", out, re.M).group(1)
            with open(log, "rb") as handle:
                data = handle.read()
            self.assertEqual(data.count(b"|BEGIN|"), 1)
            self.assertIn(b"CURIA1|9|BEGIN", data)

    def test_partial_frame_flag_and_wine_layout(self):
        with tempfile.TemporaryDirectory() as root:
            out = self.run_tool("--root", root, "--layout", "wine", "--repeat", "1", "--partial-frame")
            log = re.search(r"^LOG (.*)$", out, re.M).group(1)
            self.assertIn("pfx", log)
            with open(log, "rb") as handle:
                data = handle.read()
            self.assertEqual(data.count(b"|BEGIN|"), 2)
            self.assertEqual(data.count(b"|END|"), 1)

    def test_fixture_option(self):
        with tempfile.TemporaryDirectory() as root:
            out = self.run_tool("--root", root, "--fixture", os.path.join(FIXTURES, "example_frame.log"))
            log = re.search(r"^LOG (.*)$", out, re.M).group(1)
            with open(log, "rb") as handle:
                self.assertEqual(handle.read().count(b"\n"), 16)


class CommandTests(unittest.TestCase):
    def test_stdin_commands(self):
        out = io.StringIO()
        with fk.FakeCk3() as fake:
            fake.launch()
            commands = ["frame", "noise 2", "error", "partial", "rest", "size", "bogus", "quit", "frame"]
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                fk.run_commands(fake, commands, out)
            self.assertIn("unknown command", err.getvalue())
            data = b"\n".join(read_lines(fake))
            self.assertEqual(data.count(b"|BEGIN|"), 2)  # the frame after quit is not written
            self.assertEqual(data.count(b"|END|"), 2)
        self.assertIn("snap 1", out.getvalue())
        self.assertIn("snap 2", out.getvalue())


if __name__ == "__main__":
    unittest.main()
