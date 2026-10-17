#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The whole chain without the game and without the real service (CTest: chat_e2e and e2e_secrecy).

    fake CK3 (tools/fake_ck3)  ->  debug.log  ->  curia --headless-chat  ->  mock LLM (tools/mock_llm)

Each test starts the mock (port 0, `LISTEN host:port`), writes a log with the fake CK3 (a library
call, or its command line where the flag itself is the thing under test), starts `curia
--headless-chat --test-endpoint http://127.0.0.1:<port>` with a key that says it is fake, talks to it
through stdin and reads its JSON-lines events from stdout. Afterwards the mock's request records say
what the client really sent.

Suite `chat` (ctest chat_e2e, labels e2e and chat):
  * a click in the fake game opens the overlay, a question gets a streamed answer that quotes the
    fixture's gold and heir (the `cite` scenario reads them out of the request's realm block), the
    second question reads the prompt cache (cache_read > 0); a question before any click sends the
    "nothing has been read" block;
  * without flags the request carries Curia's default effort (low, D22) and no thinking setting;
    --effort and --thinking reach the request, `--effort default` sends no effort setting, ready and
    done report what was sent; a thinking block gives one "thinking" status before the text and a
    thinking_started_ms;
  * the newest snapshot of an existing log is the default, --frame picks another, an unknown number
    is a setup error;
  * the log going silent: with `arm`, `restart_ck3` within the short test threshold; when the game
    is told not to be running, `ck3_not_running` and never `restart_ck3`; the passive notice;
  * cancel in the middle of a stream keeps the text received and ends with exactly one terminal event
    and the socket closed; a mid-stream error keeps the partial text; a refused 307 is no answer;
  * not configured: the missing variables are named, nothing is sent; the end of stdin quits cleanly.

Suite `secrecy` (ctest e2e_secrecy, labels e2e and secrecy): a full session with a random fake key,
including a key reflected by the server (whole, and split over two stream events and two TCP pieces
at many offsets, and cut in two by the end of the stream: a cancel, a dropped connection, for a key
with and without the usual prefix), a deliberate exception and a process killed in the middle of an
answer, with transcripts switched on. (What this suite sees is the output after every layer of
redaction: the layer that redacts the controller's joined text is also covered by the unit tests of
the advisor session, because the JSON writer and the transcript writer redact again.)
Afterwards stdout, stderr, the app log, the transcripts, the whole data tree and any crash file are
scanned for the key and for every 8-character piece of its random part. The key may appear only in
the mock's records as its digest. A decoy listener and tripwire proxy variables prove that no
redirect and no proxy ever received a connection.

The children run with an allowlisted environment (PATH, the system folders Windows needs, PYTHONUTF8)
plus what the test adds. No shell, no signals other than kill(): it also runs on Windows. The test
never prints the key: on a failure it names a hash of it.

    python3 app/tests/chat_e2e.py --curia <curia> --mock tools/mock_llm/mock_llm.py \\
        --fake-ck3 tools/fake_ck3/fake_ck3.py --suite chat|secrecy
"""
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import pathlib
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from typing import Any, Dict, List, Optional, Sequence

CURIA = ""
MOCK = ""
FAKE_CK3 = ""
ml: Any = None  # the mock_llm module (for key_digest)
fk: Any = None  # the fake_ck3 module

KEY_VARIABLE = "CURIA_" + "ANTHROPIC_API_KEY"
MODEL_VARIABLE = "CURIA_" + "MODEL"
LOG_CONTENT_VARIABLE = "CURIA_" + "LOG_CONTENT"
TRANSCRIPT_VARIABLE = "CURIA_" + "TRANSCRIPT_DIR"
PROXY_VARIABLES = ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY")
SYSTEM_VARIABLES = ("PATH", "SYSTEMROOT", "SystemRoot", "WINDIR", "TEMP", "TMP", "TMPDIR")
FAST = ["--poll-ms", "25", "--tick-ms", "25"]


def fake_key() -> str:
    """A random key that says it is fake. Built at run time so no tracked file holds a key shape."""
    return "sk-" + "ant-" + "FAKE" + secrets.token_hex(12)


def key_hash(key: str) -> str:
    return hashlib.sha256(key.encode()).hexdigest()[:10]


def key_pieces(key: str) -> List[str]:
    """The key and every 8-character piece of its random part (the part after FAKE)."""
    tail = key.split("FAKE", 1)[1]
    return [key] + [tail[i:i + 8] for i in range(len(tail) - 7)]


def base_env() -> Dict[str, str]:
    env = {"PYTHONUTF8": "1"}
    for name in SYSTEM_VARIABLES:
        if name in os.environ:
            env[name] = os.environ[name]
    return env


class Listener:
    """A TCP listener on loopback that counts connections and keeps what they send."""

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.connections = 0
        self.received = b""
        self._stop = False
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def _loop(self) -> None:
        while not self._stop:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self._lock:
                self.connections += 1
            conn.settimeout(0.2)
            try:
                data = conn.recv(65536)
                with self._lock:
                    self.received += data
            except OSError:
                pass
            conn.close()

    def count(self) -> int:
        with self._lock:
            return self.connections

    def close(self) -> None:
        self._stop = True
        try:
            self.sock.close()
        except OSError:
            pass
        self._thread.join(timeout=2)


class Mock:
    """tools/mock_llm/mock_llm.py as a child process."""

    def __init__(self, extra: Sequence[str] = ()) -> None:
        self.proc = subprocess.Popen(
            [sys.executable, MOCK] + list(extra),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=base_env())
        self.output: List[str] = []
        self._listening = threading.Event()
        self.port = 0
        self._pump = threading.Thread(target=self._read, daemon=True)
        self._pump.start()
        if not self._listening.wait(20):
            self.kill()
            raise RuntimeError("the mock did not print LISTEN")

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for raw in iter(self.proc.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            self.output.append(line)
            if line.startswith("LISTEN ") and not self._listening.is_set():
                self.port = int(line.rsplit(":", 1)[1])
                self._listening.set()

    @property
    def url(self) -> str:
        return "http://127.0.0.1:%d" % self.port

    def _request(self, method: str, path: str) -> str:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)  # never uses a proxy
        try:
            conn.request(method, path)
            return conn.getresponse().read().decode("utf-8", "replace")
        finally:
            conn.close()

    def records_text(self) -> str:
        return self._request("GET", "/__mock/requests")

    def records(self) -> List[Dict[str, Any]]:
        return json.loads(self.records_text())

    def wait_record(self, index: int, timeout: float = 10.0) -> Dict[str, Any]:
        """The record of request `index` once the mock has finished with it."""
        deadline = time.monotonic() + timeout
        while True:
            records = self.records()
            if len(records) > index and records[index]["t_close"] is not None:
                return records[index]
            if time.monotonic() > deadline:
                raise AssertionError("request %d did not finish within %.0f s" % (index, timeout))
            time.sleep(0.05)

    def kill(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(timeout=10)
        self._close_pipes()

    def _close_pipes(self) -> None:
        for pipe in (self.proc.stdin, self.proc.stdout):
            try:
                if pipe is not None:
                    pipe.close()
            except OSError:
                pass

    def stop(self) -> str:
        """Asks the mock to quit (the portable way); returns everything it printed."""
        try:
            self._request("POST", "/__mock/quit")
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.kill()
        self._pump.join(timeout=5)
        self._close_pipes()
        return "\n".join(self.output)


class Headless:
    """A `curia --headless-chat` child: events are read as they come, commands go to its stdin."""

    def __init__(self, command: Sequence[str], env: Dict[str, str], cwd: str, stderr_path: str) -> None:
        self.stderr_path = stderr_path
        self.stderr_file = open(stderr_path, "wb")
        self.proc = subprocess.Popen(list(command), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.stderr_file, env=env, cwd=cwd)
        self.events: List[Dict[str, Any]] = []
        self.bad_lines: List[str] = []
        self.raw_lines: List[str] = []
        self._cond = threading.Condition()
        self._pump = threading.Thread(target=self._read, daemon=True)
        self._pump.start()

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for raw in iter(self.proc.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            with self._cond:
                self.raw_lines.append(line)
                try:
                    self.events.append(json.loads(line))
                except ValueError:
                    self.bad_lines.append("(a line that is not JSON, %d bytes)" % len(line))
                self._cond.notify_all()

    def send(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write((line + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def close_stdin(self) -> None:
        try:
            if self.proc.stdin is not None:
                self.proc.stdin.close()
        except OSError:
            pass

    def wait_event(self, predicate: Any, timeout: float = 20.0, after: int = 0) -> Dict[str, Any]:
        """The first event at or after index `after` that satisfies `predicate`."""
        deadline = time.monotonic() + timeout
        with self._cond:
            seen = after
            while True:
                while seen < len(self.events):
                    event = self.events[seen]
                    seen += 1
                    if predicate(event):
                        return event
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    kinds = [event.get("event") for event in self.events]
                    raise AssertionError("no matching event within %.0f s; events so far: %r"
                                         % (timeout, kinds))
                self._cond.wait(min(remaining, 0.2))

    def wait_kind(self, kind: str, timeout: float = 20.0, after: int = 0) -> Dict[str, Any]:
        return self.wait_event(lambda e: e.get("event") == kind, timeout, after)

    def snapshot_events(self) -> List[Dict[str, Any]]:
        with self._cond:
            return list(self.events)

    def of(self, kind: str) -> List[Dict[str, Any]]:
        return [e for e in self.snapshot_events() if e.get("event") == kind]

    def terminals(self) -> List[Dict[str, Any]]:
        return [e for e in self.snapshot_events() if e.get("event") in ("done", "cancelled", "error")]

    def wait_exit(self, timeout: float = 30.0) -> int:
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.kill()
            raise AssertionError("curia did not exit within %.0f s" % timeout)
        self._finish()
        return code

    def kill(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(timeout=10)
        self._finish()

    def _finish(self) -> None:
        self._pump.join(timeout=5)
        for pipe in (self.proc.stdin, self.proc.stdout):
            try:
                if pipe is not None:
                    pipe.close()
            except OSError:
                pass
        if not self.stderr_file.closed:
            self.stderr_file.close()

    def stderr_text(self) -> str:
        return pathlib.Path(self.stderr_path).read_bytes().decode("utf-8", "replace")

    def stdout_text(self) -> str:
        return "\n".join(self.raw_lines)


def closed_port() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


class ChatCase(unittest.TestCase):
    """Common set-up: a scratch tree that doubles as the data tree, a tripwire proxy, a mock."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.work = self.root / "work"
        self.work.mkdir()
        self.tripwire = Listener()
        self.addCleanup(self.tripwire.close)
        self.mocks: List[Mock] = []
        self.children: List[Headless] = []
        self.runs = 0

    def tearDown(self) -> None:
        try:
            for child in self.children:
                if child.proc.poll() is None:
                    child.kill()
            self.assertEqual(self.tripwire.count(), 0, "a request went through the tripwire proxy")
            self.assertEqual(self.tripwire.received, b"")
        finally:
            for mock in self.mocks:
                mock.stop()

    def assert_absent(self, needle: str, text: str, where: str) -> None:
        """Fails without printing the needle or the text (a key, game text): only a hash and where."""
        if needle in text:
            self.fail("a %d-character string (hash %s) is in %s" % (len(needle), key_hash(needle), where))

    def assert_key_absent(self, key: str, text: str, where: str) -> None:
        """The key and every 8-character piece of its random part are absent from `text`."""
        for index, piece in enumerate(key_pieces(key)):
            if piece in text:
                self.fail("piece %d (%d characters) of the key %s is in %s"
                          % (index, len(piece), key_hash(key), where))

    def start_mock(self, *extra: str) -> Mock:
        mock = Mock(extra)
        self.mocks.append(mock)
        return mock

    def env(self, key: Optional[str], model: Optional[str], extra: Optional[Dict[str, str]] = None) -> Dict[str, str]:
        env = base_env()
        proxy = "http://127.0.0.1:%d" % self.tripwire.port
        for name in PROXY_VARIABLES:
            env[name] = proxy
        # Everything the program might write by default lands in the scratch tree.
        home = str(self.home)
        env.update({"HOME": home, "USERPROFILE": home, "XDG_STATE_HOME": str(self.home / "state"),
                    "XDG_DATA_HOME": str(self.home / "data"), "LOCALAPPDATA": str(self.home / "local"),
                    "APPDATA": str(self.home / "roaming")})
        if key is not None:
            env[KEY_VARIABLE] = key
        if model is not None:
            env[MODEL_VARIABLE] = model
        env.update(extra or {})
        return env

    def start_curia(self, mock: Optional[Mock], log: str, key: Optional[str], model: Optional[str] = "mock-model-1",
                    args: Sequence[str] = (), extra_env: Optional[Dict[str, str]] = None, fast: bool = True,
                    app_log: Optional[pathlib.Path] = None) -> Headless:
        self.runs += 1
        app_log = app_log or (self.root / ("applog%d" % self.runs))
        command = [CURIA, "--headless-chat", "--log", log, "--log-dir", str(app_log), "--timeout-seconds", "120"]
        if mock is not None:
            command += ["--test-endpoint", mock.url]
        if fast:
            command += FAST
        command += list(args)
        child = Headless(command, self.env(key, model, extra_env), str(self.work),
                         str(self.root / ("stderr%d.txt" % self.runs)))
        self.children.append(child)
        return child

    def new_fake(self, **params: Any) -> Any:
        fake = fk.FakeCk3(root=str(self.root / ("ck3_%d" % len(self.children))),
                          frame_params=fk.FrameParams(**params) if params else None)
        self.addCleanup(fake.close)
        fake.launch()
        return fake

    @staticmethod
    def done_text(child: Headless, index: int = -1) -> str:
        return child.of("done")[index]["text"]


class ChatChainTest(ChatCase):
    def test_click_to_streamed_answer_quotes_the_fixture_and_the_second_ask_hits_the_cache(self) -> None:
        mock = self.start_mock("--default", "cite")
        fake = self.new_fake(gold=4321, heir="Test Heir Quartz")
        key = fake_key()
        child = self.start_curia(mock, fake.log_path, key)
        child.wait_kind("ready")
        self.assertEqual(child.of("ready")[0]["snapshot"], None)

        fake.append_frame(snap=41)  # the player clicks Advisor in the game
        opened = child.wait_kind("overlay_open")
        self.assertEqual(opened["source"], "game_click")
        snapshot = child.wait_kind("snapshot")
        self.assertEqual(snapshot["snap"], 41)
        self.assertEqual(snapshot["date"], "15 September 1066")

        child.send("ask is my heir safe?")
        done = child.wait_kind("done")
        self.assertIn("Gold is 4321.", done["text"])
        self.assertIn("Heir is Test Heir Quartz.", done["text"])
        streamed = "".join(e["text"] for e in child.of("token"))
        self.assertEqual(streamed, done["text"])
        self.assertGreater(done["usage"]["cache_write"], 0)
        self.assertEqual(done["usage"]["cache_read"], 0)
        self.assertIsNotNone(done["first_token_ms"])
        self.assertIsNotNone(done["total_ms"])
        self.assertEqual(done["stop_reason"], "end_turn")

        child.send("ask and who might rebel?")
        second = child.wait_event(lambda e: e.get("event") == "done" and e["generation"] == 2)
        self.assertGreater(second["usage"]["cache_read"], 0)

        child.close_stdin()
        self.assertEqual(child.wait_exit(), 0)
        self.assertEqual(child.bad_lines, [])
        self.assertEqual(len(child.terminals()), 2)

        # What was really sent: the fixture's numbers in the second system block, a valid request.
        first_record = mock.wait_record(0)
        self.assertEqual(first_record["violations"], [])
        system = first_record["body_json"]["system"]
        self.assertTrue(system[1]["text"].startswith("=== WHAT THE ADVISOR CAN SEE ===\n"))
        self.assertTrue(system[1]["text"].endswith("\n=== END OF WHAT THE ADVISOR CAN SEE ==="))
        self.assertIn("Treasury: 4321 gold", system[1]["text"])
        self.assertIn("Test Heir Quartz", system[1]["text"])
        self.assertIn("WHAT THE ADVISOR CAN SEE", system[0]["text"])  # the persona names the block
        # No flag was given: Curia's default (effort low, D22) was sent, and no thinking setting.
        self.assertEqual((first_record["thinking_type"], first_record["effort"]), (None, "low"))
        self.assertNotIn("thinking", first_record["body_json"])
        self.assertEqual(first_record["body_json"]["output_config"], {"effort": "low"})
        self.assertEqual((child.of("ready")[0]["effort"], child.of("ready")[0]["thinking"]), ("low", "default"))
        self.assertEqual((done["effort"], done["thinking"]), ("low", "default"))
        self.assertNotIn("thinking_started_ms", done)
        self.assertEqual(first_record["headers"]["authorization"], ml.key_digest("Bearer " + key))
        self.assert_key_absent(key, mock.records_text(), "the mock's records")
        self.assertEqual(mock.wait_record(1)["violations"], [])

    def test_a_question_before_any_click_sends_the_nothing_read_block(self) -> None:
        mock = self.start_mock("--default", "cite")
        fake = self.new_fake()
        child = self.start_curia(mock, fake.log_path, fake_key())
        self.assertIsNone(child.wait_kind("ready")["snapshot"])
        child.send("ask what now?")
        self.assertEqual(child.wait_kind("done")["text"], "Nothing has been read yet.")
        child.close_stdin()
        self.assertEqual(child.wait_exit(), 0)
        self.assertEqual(mock.wait_record(0)["violations"], [])

    def test_effort_and_thinking_reach_the_request_and_the_events(self) -> None:
        mock = self.start_mock("--default", "basic")
        fake = self.new_fake()
        fake.append_frame(snap=1)
        child = self.start_curia(mock, fake.log_path, fake_key(),
                                 args=["--from-start", "--effort", "low", "--thinking", "between_tools"])
        ready = child.wait_kind("ready")
        self.assertEqual((ready["effort"], ready["thinking"]), ("low", "between_tools"))
        child.send("ask what now?")
        done = child.wait_kind("done")
        self.assertEqual((done["effort"], done["thinking"]), ("low", "between_tools"))
        child.close_stdin()
        self.assertEqual(child.wait_exit(), 0)
        record = mock.wait_record(0)
        self.assertEqual(record["violations"], [])
        self.assertEqual((record["thinking_type"], record["effort"]), ("between_tools", "low"))
        self.assertEqual(record["body_json"]["thinking"], {"type": "between_tools"})
        self.assertEqual(record["body_json"]["output_config"], {"effort": "low"})

        # Each flag alone changes only its own setting; the effort is Curia's default (low) unless
        # --effort says otherwise, and "--effort default" sends none (what rounds 1 and 2 called the
        # default). The events say "default" for a setting that was not sent.
        cases = (
            (["--effort", "max"], (None, "max"), ("max", "default")),
            (["--thinking", "disabled"], ("disabled", "low"), ("low", "disabled")),
            (["--effort", "default"], (None, None), ("default", "default")),
            (["--effort", "default", "--thinking", "disabled"], ("disabled", None), ("default", "disabled")),
        )
        for args, expected, reported in cases:
            alone = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start"] + args)
            ready = alone.wait_kind("ready")
            alone.send("ask again?")
            done = alone.wait_kind("done")
            alone.close_stdin()
            self.assertEqual(alone.wait_exit(), 0)
            self.assertEqual((ready["effort"], ready["thinking"]), reported, args)
            self.assertEqual((done["effort"], done["thinking"]), reported, args)
            last = mock.wait_record(len(mock.records()) - 1)
            self.assertEqual((last["thinking_type"], last["effort"]), expected, args)
            self.assertEqual("output_config" in last["body_json"], expected[1] is not None, args)
            self.assertEqual("thinking" in last["body_json"], expected[0] is not None, args)
            self.assertEqual(last["violations"], [])

        # A bad value is a usage error before anything is read or sent.
        bad = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start", "--effort", "extreme"])
        self.assertEqual(bad.wait_exit(), 2)
        self.assertIn("--effort needs default, low, medium, high, xhigh or max", bad.stderr_text())
        self.assertEqual(len(mock.records()), 1 + len(cases))

    def test_a_thinking_block_gives_one_thinking_status_before_the_text(self) -> None:
        mock = self.start_mock("--default", "thinking_blocks")
        fake = self.new_fake()
        fake.append_frame(snap=1)
        child = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start"])
        child.wait_kind("ready")
        child.send("ask think first")
        done = child.wait_kind("done")
        child.send("ask and again")
        second = child.wait_event(lambda e: e.get("event") == "done" and e["generation"] == 2)
        child.close_stdin()
        self.assertEqual(child.wait_exit(), 0)
        events = child.snapshot_events()
        for answer in (done, second):
            self.assertIn("thinking_started_ms", answer)
            self.assertGreaterEqual(answer["thinking_started_ms"], 0)
            self.assertLessEqual(answer["thinking_started_ms"], answer["first_token_ms"])
        self.assertEqual(done["text"], "Test answer from the mock server.")  # the thinking is not shown
        states = [e["state"] for e in events if e.get("event") == "status"]
        self.assertEqual(states, ["waiting", "thinking", "streaming"] * 2)
        first_thinking = next(i for i, e in enumerate(events) if e.get("event") == "status" and e["state"] == "thinking")
        first_token = next(i for i, e in enumerate(events) if e.get("event") == "token")
        self.assertLess(first_thinking, first_token)
        # Without a thinking block: no "thinking" status and no thinking_started_ms.
        plain_mock = self.start_mock("--default", "basic")
        plain = self.start_curia(plain_mock, fake.log_path, fake_key(), args=["--from-start"])
        plain.wait_kind("ready")
        plain.send("ask hello")
        plain_done = plain.wait_kind("done")
        plain.close_stdin()
        self.assertEqual(plain.wait_exit(), 0)
        self.assertNotIn("thinking_started_ms", plain_done)
        self.assertEqual([e["state"] for e in plain.of("status")], ["waiting", "streaming"])

    def test_the_newest_snapshot_is_the_default_and_frame_picks_another(self) -> None:
        mock = self.start_mock("--default", "cite")
        fake = self.new_fake()
        for snap, gold in ((5, 111), (6, 222), (7, 333)):
            fake.frame_params = fk.FrameParams(gold=gold)
            fake.append_frame(snap=snap)
        key = fake_key()

        newest = self.start_curia(mock, fake.log_path, key, args=["--from-start"])
        ready = newest.wait_kind("ready")
        self.assertEqual(ready["snapshot"], 7)
        self.assertEqual(ready["frames"], 3)
        self.assertEqual(newest.of("overlay_open"), [])  # catching up with history opens nothing
        newest.send("ask what now?")
        self.assertIn("Gold is 333.", newest.wait_kind("done")["text"])
        newest.close_stdin()
        self.assertEqual(newest.wait_exit(), 0)

        chosen = self.start_curia(mock, fake.log_path, key, args=["--from-start", "--frame", "6"])
        self.assertEqual(chosen.wait_kind("ready")["snapshot"], 6)
        chosen.send("ask what now?")
        self.assertIn("Gold is 222.", chosen.wait_kind("done")["text"])
        chosen.close_stdin()
        self.assertEqual(chosen.wait_exit(), 0)

        missing = self.start_curia(mock, fake.log_path, key, args=["--from-start", "--frame", "99"])
        fatal = missing.wait_kind("fatal")
        self.assertEqual(fatal["code"], "snapshot_not_found")
        self.assertEqual(missing.wait_exit(), 2)
        self.assertEqual(len(mock.records()), 2)  # the third run sent nothing

    def test_a_log_that_goes_silent_gives_the_restart_notice_when_armed(self) -> None:
        mock = self.start_mock()
        fake = self.new_fake()
        size_probe = fk.FakeCk3(root=str(self.root / "sizeprobe"))
        self.addCleanup(size_probe.close)
        size_probe.launch()
        size_probe.append_frame(snap=1)
        frame_bytes = size_probe.size()
        # The command line flag itself: the game's log stops for good in the middle of frame 2.
        done = subprocess.run(
            [sys.executable, FAKE_CK3, "--root", str(self.root / "ck3_cap"), "--repeat", "2",
             "--die-after-bytes", str(frame_bytes + 60)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60, env=base_env())
        self.assertEqual(done.returncode, 0)
        log = [line[4:] for line in done.stdout.decode().splitlines() if line.startswith("LOG ")][0]
        self.assertEqual(pathlib.Path(log).stat().st_size, frame_bytes + 60)

        key = fake_key()
        args = ["--from-start", "--armed-wait-ms", "400", "--arm-snapshot-age-ms", "0"]
        child = self.start_curia(mock, log, key, args=args)
        ready = child.wait_kind("ready")
        self.assertEqual(ready["snapshot"], 1)  # the cut frame 2 is not a snapshot
        self.assertEqual(child.of("notice"), [])
        started = time.monotonic()
        child.send("arm")
        notice = child.wait_event(lambda e: e.get("event") == "notice" and e["kind"] == "restart_ck3", timeout=10)
        self.assertEqual(notice["kind"], "restart_ck3")
        self.assertGreaterEqual(time.monotonic() - started, 0.35)
        self.assertLess(time.monotonic() - started, 5.0)
        child.close_stdin()
        self.assertEqual(child.wait_exit(), 0)

        # The game is not running: the same silence is "CK3 not running", never "restart CK3".
        quiet = self.start_curia(mock, log, key, args=args)
        quiet.wait_kind("ready")
        quiet.send("ck3 stopped")
        quiet.send("arm")
        quiet.wait_event(lambda e: e.get("event") == "notice" and e["kind"] == "ck3_not_running")
        time.sleep(1.2)  # three times the armed wait
        kinds = [e["kind"] for e in quiet.of("notice")]
        self.assertNotIn("restart_ck3", kinds)
        self.assertEqual(kinds[-1], "ck3_not_running")
        quiet.close_stdin()
        self.assertEqual(quiet.wait_exit(), 0)

        # Told it runs again, nothing is armed any more: the notice clears.
        quiet = self.start_curia(mock, log, key, args=args)
        quiet.wait_kind("ready")
        quiet.send("ck3 stopped")
        quiet.wait_event(lambda e: e.get("event") == "notice" and e["kind"] == "ck3_not_running")
        quiet.send("ck3 running")
        quiet.wait_event(lambda e: e.get("event") == "notice" and e["kind"] == "none")
        quiet.close_stdin()
        self.assertEqual(quiet.wait_exit(), 0)
        self.assertEqual(mock.records(), [])  # no question was asked, nothing was sent

    def test_the_passive_notice_after_a_long_silence_of_a_log_that_was_alive(self) -> None:
        mock = self.start_mock()
        fake = self.new_fake()
        fake.append_frame(snap=1)
        child = self.start_curia(mock, fake.log_path, fake_key(),
                                 args=["--from-start", "--passive-silence-ms", "500"])
        child.wait_kind("ready")
        notice = child.wait_event(lambda e: e.get("event") == "notice" and e["kind"] == "silent_soft", timeout=10)
        self.assertEqual(notice["kind"], "silent_soft")
        child.send("ck3 stopped")
        child.wait_event(lambda e: e.get("event") == "notice" and e["kind"] == "ck3_not_running")
        child.close_stdin()
        self.assertEqual(child.wait_exit(), 0)

    def test_cancel_in_the_middle_of_a_stream_keeps_the_text_and_ends_once(self) -> None:
        mock = self.start_mock("--default", "hold_after_tokens")
        fake = self.new_fake()
        fake.append_frame(snap=1)
        child = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start"])
        child.wait_kind("ready")
        child.send("ask tell me everything")
        child.wait_kind("token")
        child.send("cancel")
        cancelled = child.wait_kind("cancelled")
        self.assertIn("Test token 1.", cancelled["text"])
        child.close_stdin()
        self.assertEqual(child.wait_exit(), 0)
        self.assertEqual(len(child.terminals()), 1)
        self.assertEqual(child.of("done"), [])
        self.assertEqual(child.of("error"), [])
        record = mock.wait_record(0)
        self.assertTrue(record["client_closed_early"], "the connection was not closed on cancel")

    def test_a_midstream_error_keeps_the_partial_text(self) -> None:
        mock = self.start_mock("--default", "midstream_error")
        fake = self.new_fake()
        fake.append_frame(snap=1)
        child = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start"])
        child.wait_kind("ready")
        child.send("ask hello")
        error = child.wait_kind("error")
        self.assertEqual(error["code"], "Overloaded")
        self.assertTrue(error["retryable"])
        self.assertIn("Test token 1.", error["partial_text"])
        self.assertEqual(error["partial_len"], len(error["partial_text"].encode("utf-8")))
        child.close_stdin()
        self.assertEqual(child.wait_exit(), 1)
        self.assertEqual(len(child.terminals()), 1)

    def test_the_end_of_stdin_quits_cleanly_idle_and_with_an_answer_in_flight(self) -> None:
        mock = self.start_mock()
        fake = self.new_fake()
        fake.append_frame(snap=1)
        idle = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start"])
        idle.wait_kind("ready")
        idle.close_stdin()
        self.assertEqual(idle.wait_exit(5), 0)

        busy = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start"])
        busy.wait_kind("ready")
        busy.send("ask hello")
        busy.close_stdin()  # end of input right after the question: the answer is still delivered
        self.assertEqual(busy.wait_exit(20), 0)
        self.assertEqual(len(busy.of("done")), 1)

        quitting = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start"])
        quitting.wait_kind("ready")
        quitting.send("quit")
        self.assertEqual(quitting.wait_exit(5), 0)

    def test_not_configured_names_the_variables_and_sends_nothing(self) -> None:
        mock = self.start_mock()
        fake = self.new_fake()
        fake.append_frame(snap=1)
        both = self.start_curia(mock, fake.log_path, None, model=None, args=["--from-start"])
        event = both.wait_kind("not_configured")
        self.assertEqual(sorted(event["missing"]), sorted([KEY_VARIABLE, MODEL_VARIABLE]))
        self.assertEqual(both.wait_exit(), 2)

        key = fake_key()
        no_model = self.start_curia(mock, fake.log_path, key, model=None, args=["--from-start"])
        self.assertEqual(no_model.wait_kind("not_configured")["missing"], [MODEL_VARIABLE])
        self.assertEqual(no_model.wait_exit(), 2)
        self.assert_absent("FAKE", no_model.stdout_text() + no_model.stderr_text(), "the output")

        no_key = self.start_curia(mock, fake.log_path, None, args=["--from-start"])
        self.assertEqual(no_key.wait_kind("not_configured")["missing"], [KEY_VARIABLE])
        self.assertEqual(no_key.wait_exit(), 2)
        self.assertEqual(mock.records(), [])

    def test_a_missing_log_and_bad_endpoints_are_setup_errors(self) -> None:
        mock = self.start_mock()
        key = fake_key()
        gone = self.start_curia(mock, str(self.root / "nope.log"), key, args=["--from-start"])
        self.assertEqual(gone.wait_kind("fatal")["code"], "setup")
        self.assertEqual(gone.wait_exit(), 2)
        fake = self.new_fake()
        fake.append_frame(snap=1)
        # A key without FAKE may not be sent to a test endpoint.
        plain = self.start_curia(mock, fake.log_path, "sk-" + "ant-" + "plain" + secrets.token_hex(12),
                                 args=["--from-start"])
        fatal = plain.wait_kind("fatal")
        self.assertEqual(fatal["code"], "setup")
        self.assertEqual(plain.wait_exit(), 2)
        self.assertEqual(mock.records(), [])

    def test_a_redirect_is_not_followed(self) -> None:
        decoy = Listener()
        self.addCleanup(decoy.close)
        mock = self.start_mock("--default", "redirect_307", "--decoy-url", "http://127.0.0.1:%d/" % decoy.port)
        fake = self.new_fake()
        fake.append_frame(snap=1)
        child = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start"])
        child.wait_kind("ready")
        child.send("ask hello")
        error = child.wait_kind("error")
        self.assertEqual(error["http_status"], 307)
        child.close_stdin()
        self.assertEqual(child.wait_exit(), 1)
        time.sleep(0.3)
        self.assertEqual(decoy.count(), 0, "the redirect target received a connection")


class CheckApiTest(ChatCase):
    """`curia --check-api` against the mock (the only place it runs in CI; never the real service)."""

    def run_check(self, mock: Mock, key: Optional[str], args: Sequence[str] = (), model: Optional[str] = "mock-model-1") -> Any:
        command = [CURIA, "--check-api", "--test-endpoint", mock.url, "--log-dir", str(self.root / "applog")] + list(args)
        return subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env=self.env(key, model), cwd=str(self.work), timeout=120)

    @staticmethod
    def summary(done: Any) -> Dict[str, str]:
        lines = [l for l in done.stdout.decode().splitlines() if l.startswith("CHECK_API_RESULT ")]
        assert len(lines) == 1, done.stdout
        return dict(item.split("=", 1) for item in lines[0].split()[1:])

    def test_a_good_check_with_the_cache_probe(self) -> None:
        mock = self.start_mock("--default", "cache_demo")
        key = fake_key()
        done = self.run_check(mock, key, ["--cache-probe"])
        self.assertEqual(done.returncode, 0, done.stdout.decode()[-2000:])
        summary = self.summary(done)
        self.assertEqual(summary["ok"], "true")
        self.assertEqual(summary["key"], "present")
        self.assertEqual(summary["completion"], "ok")
        self.assertEqual(summary["status"], "200")
        self.assertEqual(summary["auth"], "bearer")
        self.assertEqual(summary["cache_probe"], "hit")
        # Curia's default settings (D22): effort low, no thinking setting, in every request.
        self.assertEqual((summary["effort"], summary["thinking"]), ("low", "default"))
        # Whole lines (splitlines): the program's stdout ends its lines with CRLF on Windows.
        self.assertIn("[info] Effort and thinking: every request of this check carries effort low (Curia's default)",
                      done.stdout.decode().splitlines())
        self.assertGreater(int(summary["cache_write"]), 0)
        self.assertGreater(int(summary["cache_read"]), 0)
        text = done.stdout.decode() + done.stderr.decode()
        self.assert_key_absent(key, text, "the check's output")
        records = mock.records()
        self.assertEqual(len(records), 3)  # the tiny request and the two probe requests
        self.assertEqual(records[1]["body_sha256"], records[2]["body_sha256"])
        for record in records:
            self.assertEqual(record["violations"], [])
            self.assertEqual((record["thinking_type"], record["effort"]), (None, "low"))

    def test_the_cache_probe_says_so_when_nothing_is_cached(self) -> None:
        mock = self.start_mock("--default", "cache_demo", "--min-cacheable-tokens", "100000")
        done = self.run_check(mock, fake_key(), ["--cache-probe"])
        self.assertEqual(done.returncode, 0)
        self.assertEqual(self.summary(done)["cache_probe"], "miss")
        self.assertIn("no cache hit: prefix below the model minimum or caching not applied", done.stdout.decode())

    def test_effort_and_thinking_go_into_every_check_request(self) -> None:
        mock = self.start_mock("--default", "cache_demo")
        done = self.run_check(mock, fake_key(), ["--cache-probe", "--effort", "medium", "--thinking", "adaptive"])
        text = done.stdout.decode()
        self.assertEqual(done.returncode, 0, text[-2000:])
        summary = self.summary(done)
        self.assertEqual((summary["effort"], summary["thinking"]), ("medium", "adaptive"))
        self.assertIn("[info] Effort and thinking: every request of this check carries --effort medium "
                      "and --thinking adaptive", text)
        records = mock.records()
        self.assertEqual(len(records), 3)
        for record in records:
            self.assertEqual((record["thinking_type"], record["effort"]), ("adaptive", "medium"))
            self.assertEqual(record["violations"], [])
        # Refused (HTTP 400) with a setting: the advice names the settings sent and how to drop them.
        refused = self.start_mock("--default", "http_400")
        done = self.run_check(refused, fake_key(), ["--thinking", "disabled"])
        text = done.stdout.decode()
        self.assertEqual(done.returncode, 1, text[-1500:])
        self.assertIn("The request carried effort low (Curia's default) and --thinking disabled.", text)
        self.assertIn("only at effort high or below", text)
        self.assertIn("Run the same check with --effort default and without --thinking", text)
        only_thinking = self.start_mock("--default", "http_400")
        done = self.run_check(only_thinking, fake_key(), ["--effort", "default", "--thinking", "disabled"])
        text = done.stdout.decode()
        self.assertIn("The request carried --thinking disabled.", text)
        self.assertIn("Run the same check without --thinking (then no thinking setting is sent)", text)
        # The default check carries Curia's default effort, and says so.
        default = self.start_mock("--default", "http_400")
        done = self.run_check(default, fake_key())
        text = done.stdout.decode()
        self.assertIn("[info] Effort and thinking: every request of this check carries effort low (Curia's default)", text)
        self.assertIn("The request carried effort low (Curia's default). The model may not support that effort setting.",
                      text)
        self.assertIn("Run the same check with --effort default (then no effort setting is sent)", text)
        self.assertEqual((default.wait_record(0)["thinking_type"], default.wait_record(0)["effort"]), (None, "low"))
        # With --effort default nothing is sent, so no such line.
        plain = self.start_mock("--default", "http_400")
        done = self.run_check(plain, fake_key(), ["--effort", "default"])
        self.assertNotIn("The request carried", done.stdout.decode())
        self.assertNotIn("Effort and thinking", done.stdout.decode())
        self.assertIn(" effort=default thinking=default ", done.stdout.decode())
        self.assertEqual((plain.wait_record(0)["thinking_type"], plain.wait_record(0)["effort"]), (None, None))
        self.assertNotIn("output_config", plain.wait_record(0)["body_json"])
        # A bad value is a usage error.
        bad = subprocess.run([CURIA, "--check-api", "--thinking", "enabled"], stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env(fake_key(), "mock-model-1"),
                             cwd=str(self.work), timeout=60)
        self.assertEqual(bad.returncode, 2)
        self.assertIn("--thinking needs adaptive, between_tools or disabled", bad.stderr.decode())

    def test_an_answer_used_up_by_thinking_names_the_setting(self) -> None:
        # The thinking fills the check's small answer limit and no text comes back.
        scenario = self.root / "thinking_only.json"
        scenario.write_text(json.dumps({"steps": [{"gen": "answer", "thinking": True, "text_parts": [],
                                                   "stop_reason": "max_tokens",
                                                   "usage": {"input_tokens": 25, "output_tokens": 256}}]}))
        # Without up-front thinking (--thinking disabled) the note would not fit, so it is left out.
        cases = ((["--effort", "max"], "--effort max"), ([], "effort low (Curia's default)"), (["--effort", "default"], None),
                 (["--thinking", "disabled"], None))
        for args, named in cases:
            mock = self.start_mock("--scenario", str(scenario))
            done = self.run_check(mock, fake_key(), args)
            text = done.stdout.decode()
            self.assertEqual(done.returncode, 0, text[-1500:])
            self.assertIn("no text came back", text)
            tail = ". Thinking counts toward the check's 256-token answer limit"
            if named is None:
                self.assertNotIn(tail, text, text[-1500:])
            else:
                self.assertIn("The request carried " + named + tail, text, text[-1500:])

    def test_the_other_sign_in_header(self) -> None:
        mock = self.start_mock()
        key = fake_key()
        done = self.run_check(mock, key, ["--auth", "x-api-key"])
        self.assertEqual(done.returncode, 0)
        self.assertEqual(self.summary(done)["auth"], "x-api-key")
        record = mock.wait_record(0)
        self.assertEqual(record["headers"]["x-api-key"], ml.key_digest(key))
        self.assertNotIn("authorization", record["headers"])

    def test_failures_come_with_plain_advice(self) -> None:
        cases = (("http_401", 1, "did not accept the key"), ("http_404", 1, "model was not found"),
                 ("http_429_retry_after", 1, "too many requests"))
        for scenario, code, advice in cases:
            mock = self.start_mock("--default", scenario)
            done = self.run_check(mock, fake_key())
            text = done.stdout.decode()
            self.assertEqual(done.returncode, code, text[-1500:])
            self.assertIn(advice, text)
            self.assertEqual(self.summary(done)["completion"], "failed")
        # Nobody listens: a network failure.
        done = subprocess.run(
            [CURIA, "--check-api", "--test-endpoint", "http://127.0.0.1:%d" % closed_port(),
             "--log-dir", str(self.root / "applog")],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=self.env(fake_key(), "mock-model-1"), cwd=str(self.work), timeout=120)
        self.assertEqual(done.returncode, 1)
        self.assertIn("could not be reached", done.stdout.decode())

    def test_missing_configuration_is_exit_2_and_names_the_variables(self) -> None:
        mock = self.start_mock()
        done = self.run_check(mock, None, model=None)
        self.assertEqual(done.returncode, 2)
        text = done.stdout.decode()
        self.assertIn(KEY_VARIABLE, text)
        self.assertIn(MODEL_VARIABLE, text)
        self.assertEqual(self.summary(done)["key"], "missing")
        self.assertEqual(mock.records(), [])


class SecrecyTest(ChatCase):
    """A random fake key must appear nowhere but as a digest in the mock's records."""

    def scan(self, key: str, texts: Dict[str, str]) -> None:
        pieces = key_pieces(key)
        names: List[str] = []
        # Every file of the scratch tree (app log, transcripts, data folders, stray files).
        for path in sorted(self.root.rglob("*")):
            if path.is_file():
                data = path.read_bytes().decode("utf-8", "replace")
                texts["file " + str(path.relative_to(self.root))] = data
                lowered = path.name.lower()
                if lowered.startswith("core") or lowered.endswith((".dmp", ".crash", ".ips", ".stackdump")):
                    names.append(str(path.relative_to(self.root)))
        for where, text in texts.items():
            for index, piece in enumerate(pieces):
                if piece in text:
                    self.fail("piece %d (%d characters) of the key %s is in %s" % (index, len(piece), key_hash(key), where))
        # A crash file is scanned like everything else above, and its existence is a failure too.
        self.assertEqual(names, [], "a crash or core file was left behind")

    def test_a_full_session_with_reflections_an_exception_and_an_abort_leaks_nothing(self) -> None:
        key = fake_key()
        decoy = Listener()
        self.addCleanup(decoy.close)
        # The queue: the key reflected whole, split over two events and TCP pieces, reflected in an
        # HTTP error body, a 307 to the decoy, then an answer that never ends.
        mock = self.start_mock("--scenario", "echo_key", "--scenario", "echo_key_split:12",
                               "--scenario", "echo_key_http", "--scenario", "redirect_307",
                               "--scenario", "hold_after_tokens",
                               "--decoy-url", "http://127.0.0.1:%d/" % decoy.port)
        fake = self.new_fake(gold=777, heir="Test Heir Secret")
        fake.append_frame(snap=1)
        transcripts = self.root / "transcripts_dir"
        app_log = self.root / "applog_secrecy"
        child = self.start_curia(
            mock, fake.log_path, key, args=["--from-start"], app_log=app_log,
            extra_env={LOG_CONTENT_VARIABLE: "1", TRANSCRIPT_VARIABLE: str(transcripts)})
        child.wait_kind("ready")

        child.send("ask first, the key reflected whole")
        error = child.wait_kind("error")
        self.assertIn("[redacted]", error["partial_text"])

        child.send("ask second, the key split over two events")
        split = child.wait_event(lambda e: e.get("event") == "done")
        self.assertIn("[redacted]", split["text"])
        self.assertTrue(split["text"].startswith("The key is [redacted]"))
        self.assertTrue(split["text"].endswith(" and that is all."))
        joined = "".join(e["text"] for e in child.of("token"))
        self.assertIn("[redacted]", joined)

        child.send("ask third, the key in an error body")
        child.wait_event(lambda e: e.get("event") == "error" and e["generation"] == 3)
        child.send("ask fourth, a redirect")
        child.wait_event(lambda e: e.get("event") == "error" and e["generation"] == 4)
        mark = len(child.snapshot_events())
        child.send("ask fifth, an answer that never ends")
        child.wait_event(lambda e: e.get("event") == "token" and e["text"].startswith("Test token"), after=mark)
        time.sleep(0.2)
        child.kill()  # a deliberate abort in the middle of an answer
        self.assertEqual(child.bad_lines, [])
        self.assertTrue(list(transcripts.glob("transcript-*.txt")), "no transcript was written")

        # The self-test exception, in a session of its own, with the same key.
        crash_log = self.root / "applog_crash"
        crashing = self.start_curia(mock, fake.log_path, key, args=["--from-start", "--selftest-throw"],
                                    app_log=crash_log, model="mock:cite",
                                    extra_env={LOG_CONTENT_VARIABLE: "1", TRANSCRIPT_VARIABLE: str(transcripts)})
        crashing.wait_kind("ready")
        crashing.send("ask this ends in an exception")
        fatal = crashing.wait_kind("fatal")
        self.assertEqual(fatal["code"], "internal_error")
        self.assertEqual(crashing.wait_exit(), 4)

        # The records hold the key only as the digest of the header value.
        records_text = mock.records_text()
        self.assert_key_absent(key, records_text, "the mock's records")
        self.assertIn(ml.key_digest("Bearer " + key), records_text)
        self.assertEqual(decoy.count(), 0, "the redirect target received a connection")
        self.assertEqual(self.tripwire.count(), 0)

        mock_output = mock.stop()
        self.scan(key, {
            "the first run's stdout": child.stdout_text(),
            "the first run's stderr": child.stderr_text(),
            "the self-test run's stdout": crashing.stdout_text(),
            "the self-test run's stderr": crashing.stderr_text(),
            "the mock's output": mock_output,
            "the mock's records": records_text,
            "the decoy's data": decoy.received.decode("latin-1"),
        })
        # The reflections were scrubbed in the transcripts, which hold the (redacted) texts, and the
        # app log holds no question and no answer at all.
        transcript_text = "\n".join(p.read_text(encoding="utf-8") for p in sorted(transcripts.glob("transcript-*.txt")))
        self.assertIn("The key is [redacted]", transcript_text)
        self.assertIn("--- question ---\nsecond, the key split over two events", transcript_text)
        app_log_text = (app_log / "curia.log").read_text(encoding="utf-8")
        for fragment in ("key split", "key reflected", "Test Heir Secret", "that is all"):
            self.assert_absent(fragment, app_log_text, "the app log")
        self.mocks.remove(mock)

    def split_session(self, key: str, label: str) -> None:
        """One session per key: the key split at many offsets, and cut in two by the end of the
        stream (a cancel, a dropped connection). Nothing of it may reach any output."""
        offsets = [1, 4, 7, 11, 12, 16, 17, 20, 26, 30, len(key) - 1]
        queue: List[str] = []
        for offset in offsets:
            queue += ["--scenario", "echo_key_split:%d" % offset]
        queue += ["--scenario", "echo_key_half:20", "--scenario", "echo_key_half:%d" % (len(key) - 3),
                  "--scenario", "echo_key_half_close:20", "--scenario", "echo_key_half_close:%d" % (len(key) - 3)]
        mock = self.start_mock(*queue)
        fake = self.new_fake()
        fake.append_frame(snap=1)
        transcripts = self.root / (label + "_transcripts")
        child = self.start_curia(mock, fake.log_path, key, args=["--from-start"],
                                 app_log=self.root / (label + "_applog"),
                                 extra_env={LOG_CONTENT_VARIABLE: "1", TRANSCRIPT_VARIABLE: str(transcripts)})
        child.wait_kind("ready")
        terminal = ("done", "cancelled", "error")
        asked = 0

        def ask(text: str) -> int:
            nonlocal asked
            asked += 1
            mark = len(child.snapshot_events())
            child.send("ask " + text)
            return mark

        for offset in offsets:
            mark = ask("the key split after %d characters" % offset)
            end = child.wait_event(lambda e: e.get("event") in terminal, after=mark)
            self.assertEqual(end["event"], "done", "offset %d" % offset)
            self.assertTrue(end["text"].startswith("The key is [redacted]"), "offset %d" % offset)
            self.assertTrue(end["text"].endswith(" and that is all."), "offset %d" % offset)
            self.assert_key_absent(key, end["text"], "the answer split at offset %d" % offset)

        for tail in (20, len(key) - 3):
            # The stream stops after the first part of the key and waits: the player cancels.
            mark = ask("the first %d characters, then cancel" % tail)
            child.wait_event(lambda e: e.get("event") == "status" and e.get("state") == "streaming", after=mark)
            child.send("cancel")
            end = child.wait_event(lambda e: e.get("event") in terminal, after=mark)
            self.assertEqual(end["event"], "cancelled")
            self.assertTrue(end["text"].startswith("The key is"), "cancel after %d" % tail)
            self.assert_key_absent(key, end["text"], "the cancelled answer (%d characters)" % tail)

        for tail in (20, len(key) - 3):
            # The connection is cut after the first part of the key: an error with the partial text.
            mark = ask("the first %d characters, then the connection drops" % tail)
            end = child.wait_event(lambda e: e.get("event") in terminal, after=mark)
            self.assertEqual(end["event"], "error")
            self.assert_key_absent(key, end["partial_text"], "the partial text (%d characters)" % tail)
            self.assert_key_absent(key, end["message"] + end["detail"], "the error text")

        child.close_stdin()
        self.assertEqual(child.wait_exit(), 1)  # the last two questions ended in an error
        self.assertEqual(child.bad_lines, [])
        records_text = mock.records_text()
        mock_output = mock.stop()
        self.mocks.remove(mock)
        self.assert_key_absent(key, records_text, "the mock's records")
        self.assertEqual(len(list(transcripts.glob("transcript-*.txt"))), asked)
        self.scan(key, {
            "the session's stdout (%s)" % label: child.stdout_text(),
            "the session's stderr (%s)" % label: child.stderr_text(),
            "the mock's output (%s)" % label: mock_output,
            "the mock's records (%s)" % label: records_text,
        })

    def test_a_key_split_anywhere_or_cut_by_the_end_of_the_stream_leaks_nothing(self) -> None:
        # The usual key, and a gateway-style one without the "sk-ant-" prefix (only the registered
        # secret finds it).
        self.split_session(fake_key(), "usual")
        self.split_session("FAKE" + "gw" + secrets.token_hex(12), "gateway")

    def test_the_default_transcript_folder_is_outside_the_working_tree(self) -> None:
        key = fake_key()
        mock = self.start_mock("--default", "cite")
        fake = self.new_fake()
        fake.append_frame(snap=1)
        child = self.start_curia(mock, fake.log_path, key, args=["--from-start"],
                                 extra_env={LOG_CONTENT_VARIABLE: "1"})
        ready = child.wait_kind("ready")
        expected = self.home / "curia_m4_results" / "transcripts"
        self.assertEqual(pathlib.Path(ready["transcripts"]).resolve(), expected.resolve())
        child.send("ask hello")
        child.wait_kind("done")
        child.close_stdin()
        self.assertEqual(child.wait_exit(), 0)
        files = list(expected.glob("transcript-*.txt"))
        self.assertEqual(len(files), 1)
        text = files[0].read_text(encoding="utf-8")
        self.assertIn("--- question ---\nhello\n", text)
        self.assertIn("Gold is 312.", text)
        self.assert_key_absent(key, text, "the transcript")
        if os.name == "posix":
            self.assertEqual(files[0].stat().st_mode & 0o777, 0o600)
            self.assertEqual(expected.stat().st_mode & 0o777, 0o700)

    def test_a_transcript_folder_inside_a_source_tree_is_refused_and_nothing_is_sent(self) -> None:
        mock = self.start_mock()
        repo = self.root / "checkout"
        (repo / ".git").mkdir(parents=True)
        fake = self.new_fake()
        fake.append_frame(snap=1)
        child = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start"],
                                 extra_env={LOG_CONTENT_VARIABLE: "1",
                                            TRANSCRIPT_VARIABLE: str(repo / "docs" / "transcripts")})
        fatal = child.wait_kind("fatal")
        self.assertEqual(fatal["code"], "transcript_refused")
        self.assertEqual(child.wait_exit(), 2)
        self.assertFalse((repo / "docs").exists())
        self.assertEqual(mock.records(), [])

    def test_content_logging_is_off_unless_asked_for(self) -> None:
        mock = self.start_mock()
        fake = self.new_fake()
        fake.append_frame(snap=1)
        child = self.start_curia(mock, fake.log_path, fake_key(), args=["--from-start"])
        child.wait_kind("ready")
        child.send("ask hello")
        child.wait_kind("done")
        child.close_stdin()
        self.assertEqual(child.wait_exit(), 0)
        self.assertEqual(child.of("transcript"), [])
        self.assertFalse((self.home / "curia_m4_results").exists())
        # The app log holds no question and no answer even so.
        applog = (self.root / "applog1" / "curia.log").read_text(encoding="utf-8")
        self.assert_absent("hello", applog, "the app log")
        self.assert_absent("Test Heir", applog, "the app log")


def main() -> int:
    global CURIA, MOCK, FAKE_CK3, ml, fk
    parser = argparse.ArgumentParser()
    parser.add_argument("--curia", required=True)
    parser.add_argument("--mock", required=True)
    parser.add_argument("--fake-ck3", required=True)
    parser.add_argument("--suite", choices=("chat", "secrecy"), required=True)
    args, rest = parser.parse_known_args()
    CURIA, MOCK, FAKE_CK3 = (os.path.abspath(args.curia), os.path.abspath(args.mock),
                             os.path.abspath(args.fake_ck3))
    sys.path.insert(0, str(pathlib.Path(MOCK).parent))
    sys.path.insert(0, str(pathlib.Path(FAKE_CK3).parent))
    import mock_llm  # noqa: E402
    import fake_ck3  # noqa: E402
    ml, fk = mock_llm, fake_ck3
    classes = {"chat": (ChatChainTest, CheckApiTest), "secrecy": (SecrecyTest,)}[args.suite]
    suite = unittest.TestSuite()
    for cls in classes:
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(cls))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
