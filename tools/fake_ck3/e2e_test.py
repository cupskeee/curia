#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""End-to-end test: fake CK3 -> `curia --watch-log` -> JSON events compared with the golden files.

Each scenario starts the fake game (tools/fake_ck3/fake_ck3.py used as a library) and the real
curia binary in its headless watch mode on the fake's log, drives the fake, collects the JSON lines
curia prints and compares them with tests/fixtures/curia1/<name>.events.json. The engine prefix and
the noise lines differ from the fixtures' own harness, and the comparison must not care: for damaged
frames, unsupported-format reports and diagnostics only the event kind, snap and reason (or leading
fields) are compared, never the human-readable detail text.

Every scenario ends with a good "sentinel" frame, so that curia's exit after N complete frames also
proves that everything before it was processed and nothing was lost behind it. The mock-LLM leg of
the end-to-end chain (tools/mock_llm) arrives with M4/M5 and is not part of this test.

Run (CTest does this as e2e_fake_ck3):
    python3 tools/fake_ck3/e2e_test.py --curia <path to curia> --fixtures tests/fixtures/curia1
"""
from __future__ import annotations

import argparse
import concurrent.futures
import copy
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from typing import Any, Callable, Dict, List, Optional

import fake_ck3 as fk

CURIA = ""
FIXTURES = ""
SENTINEL_SNAP = 4000
WAIT = 40.0  # seconds any single wait may take; the polling interval of curia is 250 ms

OFFSET_LINE = re.compile(r"^curia: offset (\d+) total (\d+) curia (\d+)$")


class Timeout(AssertionError):
    pass


class CuriaProcess:
    """`curia --watch-log` as a child process, with its stdout and stderr collected by threads."""

    def __init__(self, log_path: str, max_frames: int, from_start: bool = False):
        cmd = [CURIA, "--watch-log", log_path, "--max-frames", str(max_frames),
               "--timeout-seconds", str(int(WAIT) + 20)]
        if from_start:
            cmd.append("--from-start")
        self._cond = threading.Condition()
        self.out: List[str] = []
        self.err: List[str] = []
        self.offset = 0
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self._threads = [
            threading.Thread(target=self._pump, args=(self.proc.stdout, self.out, False), daemon=True),
            threading.Thread(target=self._pump, args=(self.proc.stderr, self.err, True), daemon=True),
        ]
        for thread in self._threads:
            thread.start()

    def _pump(self, stream, sink: List[str], track: bool) -> None:
        for raw in iter(stream.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            with self._cond:
                sink.append(line)
                if track:
                    match = OFFSET_LINE.match(line)
                    if match:
                        self.offset = int(match.group(1))
                self._cond.notify_all()

    def _wait(self, predicate: Callable[[], bool], what: str) -> None:
        deadline = time.monotonic() + WAIT
        with self._cond:
            while not predicate():
                left = deadline - time.monotonic()
                if left <= 0:
                    raise Timeout("timed out waiting for %s; stderr tail: %s" % (what, self.err[-6:]))
                self._cond.wait(min(left, 0.2))

    def count_notice(self, name: str) -> int:
        with self._cond:
            return sum(1 for line in self.err if line == "curia: log " + name)

    def wait_notice(self, name: str, count: int = 1) -> None:
        self._wait(lambda: self.count_notice(name) >= count, "'log %s' x%d" % (name, count))

    def wait_offset(self, size: int) -> int:
        """Waits until curia has read up to `size` bytes; returns the offset."""
        self._wait(lambda: self.offset == size, "offset %d (at %d)" % (size, self.offset))
        return self.offset

    def events_so_far(self) -> List[Dict[str, Any]]:
        with self._cond:
            return [json.loads(line) for line in self.out if line.strip()]

    def finish(self) -> List[Dict[str, Any]]:
        """Waits for curia to exit (it does after the asked-for number of frames) and returns its
        events. Fails if the exit code is not 0."""
        try:
            code = self.proc.wait(timeout=WAIT)
        except subprocess.TimeoutExpired:
            self.kill()
            raise Timeout("curia did not exit; stdout: %d lines, stderr tail: %s" % (len(self.out), self.err[-6:]))
        self._close_pipes()
        if code != 0:
            raise AssertionError("curia exited with %d; stderr tail: %s" % (code, self.err[-6:]))
        return self.events_so_far()

    def kill(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self._close_pipes()

    def _close_pipes(self) -> None:
        for thread in self._threads:
            thread.join(5)
        for stream in (self.proc.stdout, self.proc.stderr):
            if stream is not None:
                stream.close()


class TruncationSync:
    """The `between` hook of FakeCk3.play_fixture: lets curia catch up before the log is emptied
    and notice the truncation before more bytes arrive (what the real game's launch gives it)."""

    def __init__(self, proc: CuriaProcess, fake: fk.FakeCk3):
        self.proc = proc
        self.fake = fake
        self._offset_before = 0
        self._truncations_before = 0

    def __call__(self, kind: str) -> None:
        if kind == "truncate":
            self._offset_before = self.proc.wait_offset(self.fake.size())
            self._truncations_before = self.proc.count_notice("truncated")
        elif kind == "truncated" and self._offset_before > 0:
            # With nothing read yet there is no shrinking to see.
            self.proc.wait_notice("truncated", self._truncations_before + 1)


# -- golden files ----------------------------------------------------------------------------


def load_golden(name: str) -> List[Dict[str, Any]]:
    with open(os.path.join(FIXTURES, name + ".events.json"), encoding="utf-8") as handle:
        return json.load(handle)


def with_snap(event: Dict[str, Any], snap: int) -> Dict[str, Any]:
    event = copy.deepcopy(event)
    event["frame"]["snap"] = snap
    return event


def example_event(snap: int) -> Dict[str, Any]:
    (event,) = load_golden("example_frame")
    return with_snap(event, snap)


def normalize(event: Dict[str, Any]) -> Dict[str, Any]:
    """What must match: everything except the human-readable `detail` of reports."""
    event = copy.deepcopy(event)
    if event["event"] in ("frame_damaged", "diagnostic"):
        event.pop("detail", None)
    return event


def check_events(actual: List[Dict[str, Any]], expected: List[Dict[str, Any]]) -> None:
    a = [normalize(e) for e in actual]
    e = [normalize(x) for x in expected]
    if a != e:
        for index in range(max(len(a), len(e))):
            left = a[index] if index < len(a) else None
            right = e[index] if index < len(e) else None
            if left != right:
                raise AssertionError("event %d differs\n  actual:   %s\n  expected: %s"
                                     % (index, json.dumps(left, sort_keys=True)[:600],
                                        json.dumps(right, sort_keys=True)[:600]))


def frames_in(events: List[Dict[str, Any]]) -> int:
    return sum(1 for e in events if e["event"] == "frame_complete")


# -- scenarios (each returns nothing and raises on failure; they run in parallel) ------------------


def run_fixture(name: str, **fake_options: Any) -> None:
    """Plays one fixture through the fake and a sentinel frame behind it."""
    expected = load_golden(name) + [example_event(SENTINEL_SNAP)]
    with fk.FakeCk3(**fake_options) as fake:
        fake.launch()
        proc = CuriaProcess(fake.log_path, frames_in(expected))
        try:
            proc.wait_notice("attached")
            fake.play_fixture(os.path.join(FIXTURES, name + ".log"), between=TruncationSync(proc, fake))
            fake.append_frame(snap=SENTINEL_SNAP)
            check_events(proc.finish(), expected)
        finally:
            proc.kill()


def run_example(fake_options: Optional[Dict[str, Any]] = None, snaps: Optional[List[int]] = None) -> None:
    snaps = snaps or [7]
    expected = [example_event(s) for s in snaps] + [example_event(SENTINEL_SNAP)]
    with fk.FakeCk3(**(fake_options or {})) as fake:
        fake.launch()
        proc = CuriaProcess(fake.log_path, len(expected))
        try:
            proc.wait_notice("attached")
            fake.append_noise(2)
            for snap in snaps:
                fake.append_frame(snap=snap)
            fake.append_noise(1)
            fake.append_frame(snap=SENTINEL_SNAP)
            check_events(proc.finish(), expected)
        finally:
            proc.kill()


def run_partial_then_complete() -> None:
    expected = [example_event(7), example_event(SENTINEL_SNAP)]
    with fk.FakeCk3() as fake:
        fake.launch()
        proc = CuriaProcess(fake.log_path, 2)
        try:
            proc.wait_notice("attached")
            fake.append_frame(snap=7, partial=True)
            proc.wait_offset(fake.size())
            time.sleep(0.6)  # more than two polls: a frame without END must not be reported
            if proc.events_so_far():
                raise AssertionError("events before END: %s" % proc.events_so_far())
            fake.finish_frame()
            fake.append_frame(snap=SENTINEL_SNAP)
            check_events(proc.finish(), expected)
        finally:
            proc.kill()


def run_truncation_at_launch(open_frame: bool) -> None:
    """History in the log, then the game "launches" again: the log is emptied in place."""
    expected: List[Dict[str, Any]] = []
    if open_frame:
        expected.append({"event": "frame_damaged", "snap": 7, "reason": "truncated_log"})
    expected += [example_event(1), example_event(SENTINEL_SNAP)]
    with fk.FakeCk3() as fake:
        fake.launch()
        for snap in (5, 6):
            fake.append_frame(snap=snap)  # an earlier session, before curia runs
        proc = CuriaProcess(fake.log_path, 2)
        try:
            proc.wait_notice("attached")
            if open_frame:
                fake.append_frame(snap=7, partial=True)
            proc.wait_offset(fake.size())
            fake.launch(truncate=True)  # same file, size back to 0
            proc.wait_notice("truncated")
            fake.append_frame(snap=1)
            fake.append_frame(snap=SENTINEL_SNAP)
            check_events(proc.finish(), expected)
        finally:
            proc.kill()


def run_attach_to_existing() -> None:
    """History is not replayed: only what is appended after curia attached is read."""
    with fk.FakeCk3() as fake:
        fake.launch()
        for snap in (1, 2):
            fake.append_frame(snap=snap)
        proc = CuriaProcess(fake.log_path, 1)
        try:
            proc.wait_notice("attached")
            proc.wait_offset(fake.size())
            fake.append_frame(snap=3)
            events = proc.finish()
            check_events(events, [example_event(3)])
        finally:
            proc.kill()


def run_log_appears_later() -> None:
    """The log does not exist when curia starts; a file that appears is read from its first byte."""
    expected = [example_event(7), example_event(SENTINEL_SNAP)]
    with fk.FakeCk3(layout="wine") as fake:
        proc = CuriaProcess(fake.log_path, 2)  # the folder does not exist yet
        try:
            proc.wait_notice("missing")
            fake.launch()
            fake.append_noise(3)
            fake.append_frame(snap=7)
            fake.append_frame(snap=SENTINEL_SNAP)
            check_events(proc.finish(), expected)
            if proc.count_notice("attached") != 1:
                raise AssertionError("expected one attach, stderr: %s" % proc.err[:6])
        finally:
            proc.kill()


def run_load_lines() -> None:
    expected = [example_event(7), example_event(SENTINEL_SNAP)]
    with fk.FakeCk3() as fake:
        fake.launch()
        proc = CuriaProcess(fake.log_path, 2)
        try:
            proc.wait_notice("attached")
            fake.append_records([b"CURIA1|0|LOAD|0.1.0"])
            fake.append_frame(snap=7)
            fake.append_records([b"CURIA1|0|LOAD|0.1.0"])
            fake.append_frame(snap=SENTINEL_SNAP)
            check_events(proc.finish(), expected)
        finally:
            proc.kill()


class EndToEnd(unittest.TestCase):
    def test_basic_frame(self):
        run_example()

    def test_interleaved_noise_and_error_entries(self):
        run_example({"interleave": True})

    def test_noise_fixture(self):
        run_fixture("noise_interleaved")

    def test_burst_and_per_line_writes(self):
        run_example({"burst": True})
        run_example({"burst": False})

    def test_lines_written_in_two_chunks(self):
        run_example({"line_buffer": True})

    def test_delayed_flush(self):
        run_example({"flush_delay": 0.03})

    def test_two_frames_in_a_row(self):
        run_example(snaps=[7, 8])

    def test_wine_layout_with_spaces_in_the_path(self):
        run_example({"layout": "wine"})

    def test_partial_frame_then_the_rest(self):
        run_partial_then_complete()

    def test_truncation_at_launch_between_frames(self):
        run_truncation_at_launch(open_frame=False)

    def test_truncation_at_launch_with_an_open_frame(self):
        run_truncation_at_launch(open_frame=True)

    def test_oversize_name_keeps_the_frame_and_flags_it(self):
        run_fixture("oversize_name")
        golden = load_golden("oversize_name")
        self.assertTrue(golden[0]["frame"]["text_shortened"])

    def test_damaged_frame_is_reported(self):
        run_fixture("damaged_count_mismatch")
        self.assertEqual(load_golden("damaged_count_mismatch")[0]["reason"], "count_mismatch")

    def test_attach_to_an_existing_log_does_not_replay_history(self):
        run_attach_to_existing()

    def test_log_that_appears_later(self):
        run_log_appears_later()

    def test_load_lines_are_ignored(self):
        run_load_lines()

    def test_every_fixture(self):
        names = sorted(f[:-4] for f in os.listdir(FIXTURES) if f.endswith(".log"))
        self.assertGreater(len(names), 20)
        failures: List[str] = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            futures = {pool.submit(run_fixture, name): name for name in names}
            for future in concurrent.futures.as_completed(futures):
                try:
                    future.result()
                except Exception as exc:  # noqa: BLE001 - collect every failing fixture
                    failures.append("%s: %s" % (futures[future], exc))
        self.assertEqual(failures, [], "\n".join(sorted(failures)))


def main() -> int:
    global CURIA, FIXTURES
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--curia", required=True, help="path to the curia executable")
    parser.add_argument("--fixtures", required=True, help="tests/fixtures/curia1")
    args, rest = parser.parse_known_args()
    CURIA = os.path.abspath(args.curia)
    FIXTURES = os.path.abspath(args.fixtures)
    if not os.path.isfile(CURIA):
        print("e2e_test: no such executable: %s" % CURIA, file=sys.stderr)
        return 2
    if not os.path.isdir(FIXTURES):
        print("e2e_test: no such fixtures folder: %s" % FIXTURES, file=sys.stderr)
        return 2
    result = unittest.main(argv=[sys.argv[0]] + rest, exit=False, verbosity=2).result
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
