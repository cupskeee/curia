#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The Anthropic provider against the mock server (CTest: provider_mock, labels e2e and provider).

Each test starts tools/mock_llm/mock_llm.py (port 0, the OS chooses; the first stdout line is
`LISTEN host:port`), runs `curia_chat_probe` against it as a child process and reads the probe's JSON
lines. Afterwards it reads the mock's request records (GET /__mock/requests) to check what the
client really sent and when it closed the socket.

What is proved (the numbers refer to docs/architecture.md section 7):
  * stream shapes: a plain answer, ping and unknown events, CRLF, one byte per TCP send (also inside
    a UTF-8 sequence), \\u escapes, `Connection: close` framing, a thinking block, a refusal, a cut
    off by max_tokens, a mid-stream error event, a truncated stream (partial text kept), an RST;
  * every HTTP status the API documents maps to the right code, retry flag, Retry-After and request id;
  * the stall guard fires with a short test timeout and does not hang; a cancel (by token count or by
    time) ends the request with the partial text AND closes the TCP connection (the mock sees the
    client go; A11 at the socket level); destroying the provider mid-stream calls no handler;
  * secrecy: a random fake key reflected by the server (echo_key, echo_key_http) appears nowhere in
    the probe's stdout or stderr, the app log or the mock's records and output, except as the mock's
    own digest; the app log holds no prompt or answer text; a 307 is not followed (the decoy
    listener sees no connection); a proxy named in the environment is never used for loopback; a
    non-loopback or non-http test endpoint, and a key without FAKE, are refused before any request;
  * a cancelled or destroyed request closes its own connection: the probe stays alive afterwards
    (--linger-ms) and the mock must already have seen the close, so the process exit proves nothing
    here; a cancel from another thread is reported within a fraction of a second (the net thread is
    woken, not left to a poll timeout);
  * a 200 that is not an event stream (a login page) and a server that does not speak HTTP get their
    own, honest messages; a connected but silent server is a stall, not a connect timeout;
  * the x-api-key fallback sends that header and not the other; two runs send identical body bytes
    (the cache prefix is deterministic) and the cache emulation then reports cache_read.

The probe runs with an allowlisted environment (PATH, the system folders Windows needs, PYTHONUTF8)
plus the key and tripwire proxy variables that point at a listener which counts connections, so a
real call (or any use of a proxy) cannot go unnoticed. No shell, no signals: it also runs on Windows.

    python3 app/tests/provider_mock.py --probe <curia_chat_probe> --mock tools/mock_llm/mock_llm.py
"""
from __future__ import annotations

import argparse
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
from typing import Any, Dict, List, Optional, Sequence, Tuple

PROBE = ""
MOCK = ""
ml: Any = None  # the mock_llm module, imported in main() (for key_digest)

KEY_VARIABLE = "CURIA_" + "ANTHROPIC_API_KEY"
PROXY_VARIABLES = ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY")
SYSTEM_VARIABLES = ("PATH", "SYSTEMROOT", "SystemRoot", "WINDIR", "TEMP", "TMP", "TMPDIR")


def fake_key() -> str:
    """A random key that says it is fake. Built at run time so no tracked file holds a key shape."""
    return "sk-" + "ant-" + "FAKE" + secrets.token_hex(12)


def real_looking_key() -> str:
    """A random key without the FAKE marker (the probe must refuse it for a test endpoint)."""
    return "sk-" + "ant-" + "plain" + secrets.token_hex(12)


def base_env() -> Dict[str, str]:
    env = {"PYTHONUTF8": "1"}
    for name in SYSTEM_VARIABLES:
        if name in os.environ:
            env[name] = os.environ[name]
    return env


class Listener:
    """A TCP listener on loopback that accepts connections, counts them and keeps what they send."""

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


class RawServer:
    """A loopback server that answers every connection with fixed bytes (or nothing), for the cases
    the mock cannot play: garbage that is not HTTP, a server that never answers."""

    def __init__(self, response: bytes, hold: bool = False) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.response = response
        self.hold = hold
        self.closed_early = threading.Event()
        self._stop = False
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
            conn.settimeout(0.2)
            try:
                head = b""
                while b"\r\n\r\n" not in head and not self._stop:
                    try:
                        chunk = conn.recv(65536)
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    head += chunk
                if self.response:
                    conn.sendall(self.response)
                if self.hold:
                    deadline = time.monotonic() + 30
                    while not self._stop and time.monotonic() < deadline:
                        try:
                            if not conn.recv(65536):
                                self.closed_early.set()
                                break
                        except socket.timeout:
                            continue
            except OSError:
                self.closed_early.set()
            finally:
                conn.close()

    @property
    def url(self) -> str:
        return "http://127.0.0.1:%d" % self.port

    def close(self) -> None:
        self._stop = True
        try:
            self.sock.close()
        except OSError:
            pass
        self._thread.join(timeout=3)


class LiveProbe:
    """A probe child process that is read while it runs (the probe's --linger-ms keeps it alive after
    the request ended, so a test can look at the mock while the process still exists)."""

    def __init__(self, command: Sequence[str], env: Dict[str, str]) -> None:
        self.stderr_file = tempfile.TemporaryFile()
        self.proc = subprocess.Popen(list(command), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=self.stderr_file, env=env)
        self.lines: List[str] = []
        self._lingering = threading.Event()
        self._pump = threading.Thread(target=self._read, daemon=True)
        self._pump.start()

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for raw in iter(self.proc.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            self.lines.append(line)
            if '"event":"lingering"' in line.replace(" ", ""):
                self._lingering.set()

    def wait_lingering(self, timeout: float) -> bool:
        return self._lingering.wait(timeout)

    def alive(self) -> bool:
        return self.proc.poll() is None

    def kill(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(timeout=10)
        self._pump.join(timeout=5)
        try:
            if self.proc.stdout is not None:
                self.proc.stdout.close()
        except OSError:
            pass
        self.stderr_file.close()


def closed_port() -> int:
    """A loopback port nothing listens on (bound, read and released)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


class Mock:
    """tools/mock_llm/mock_llm.py as a child process."""

    def __init__(self, extra: Sequence[str] = ()) -> None:
        self.proc = subprocess.Popen(
            [sys.executable, MOCK] + list(extra),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=base_env())
        self.output: List[str] = []
        self._listening = threading.Event()
        self._done = threading.Event()
        self.port = 0
        self._pump = threading.Thread(target=self._read, daemon=True)
        self._pump.start()
        if not self._listening.wait(20):
            self.kill()
            raise RuntimeError("the mock did not print LISTEN; output: %r" % self.output)

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for raw in iter(self.proc.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            self.output.append(line)
            if line.startswith("LISTEN ") and not self._listening.is_set():
                self.port = int(line.rsplit(":", 1)[1])
                self._listening.set()
            elif line == "DONE":
                self._done.set()

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
                raise AssertionError("request %d did not finish within %.0f s: %r" % (index, timeout, records))
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
        """Asks the mock to quit (the portable way) and returns everything it printed."""
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


class ProbeRun:
    def __init__(self, completed: "subprocess.CompletedProcess[bytes]", seconds: float, log_dir: pathlib.Path) -> None:
        self.returncode = completed.returncode
        self.stdout = completed.stdout.decode("utf-8", "replace")
        self.stderr = completed.stderr.decode("utf-8", "replace")
        self.seconds = seconds
        self.log_dir = log_dir
        self.events: List[Dict[str, Any]] = []
        for line in self.stdout.splitlines():
            if line.strip():
                self.events.append(json.loads(line))

    def of(self, kind: str) -> List[Dict[str, Any]]:
        return [event for event in self.events if event["event"] == kind]

    @property
    def text(self) -> str:
        return "".join(event["text"] for event in self.of("token") if event["kind"] == "text")

    @property
    def last(self) -> Dict[str, Any]:
        return self.events[-1]

    @property
    def done(self) -> Dict[str, Any]:
        found = self.of("done")
        assert len(found) == 1, self.stdout
        return found[0]

    @property
    def error(self) -> Dict[str, Any]:
        found = self.of("error")
        assert len(found) == 1, self.stdout
        return found[0]

    def log_text(self) -> str:
        parts = []
        if self.log_dir.exists():
            for path in sorted(self.log_dir.rglob("*")):
                if path.is_file():
                    parts.append(path.read_bytes().decode("utf-8", "replace"))
        return "\n".join(parts)


class ProviderMockTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.tripwire = Listener()
        self.addCleanup(self.tripwire.close)
        self.mocks: List[Mock] = []
        self.runs = 0

    def tearDown(self) -> None:
        # No probe run may have used the proxy named in its environment.
        self.assertEqual(self.tripwire.count(), 0, "a request went through the tripwire proxy")
        self.assertEqual(self.tripwire.received, b"")
        for mock in self.mocks:
            mock.stop()

    # -- helpers ---------------------------------------------------------------------------------

    def start_mock(self, *extra: str) -> Mock:
        mock = Mock(extra)
        self.mocks.append(mock)
        return mock

    def probe_command(self, endpoint: Optional[str], model: str, key: Optional[str], prompt: str,
                      args: Sequence[str], env_key: bool) -> Tuple[List[str], Dict[str, str], pathlib.Path]:
        self.runs += 1
        log_dir = pathlib.Path(self.tmp.name) / ("applog%d" % self.runs)
        command = [PROBE]
        if endpoint is not None:
            command += ["--test-endpoint", endpoint]
        command += ["--model", model, "--prompt", prompt, "--log-dir", str(log_dir)] + list(args)
        env = base_env()
        proxy = "http://127.0.0.1:%d" % self.tripwire.port
        for name in PROXY_VARIABLES:
            env[name] = proxy
        if env_key:
            env[KEY_VARIABLE] = key if key is not None else fake_key()
        return command, env, log_dir

    def probe(self, endpoint: Optional[str], model: str = "mock:basic", key: Optional[str] = None,
              prompt: str = "hello", args: Sequence[str] = (), timeout: float = 90.0,
              env_key: bool = True) -> ProbeRun:
        command, env, log_dir = self.probe_command(endpoint, model, key, prompt, args, env_key)
        started = time.monotonic()
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, env=env,
                                   timeout=timeout)
        return ProbeRun(completed, time.monotonic() - started, log_dir)

    def probe_live(self, mock: Mock, scenario: str, args: Sequence[str]) -> LiveProbe:
        """Starts the probe against `mock` with --linger-ms and returns at once."""
        command, env, _ = self.probe_command(mock.url, "mock:" + scenario, None, "hello",
                                             list(args) + ["--linger-ms", "15000"], True)
        live = LiveProbe(command, env)
        self.addCleanup(live.kill)
        return live

    def assert_socket_closed_while_probe_lives(self, live: LiveProbe, mock: Mock) -> None:
        """The request has ended and the probe lingers: the mock must already have seen the client go
        (or see it within a second or so), with the process still running. Exiting is not closing."""
        self.assertTrue(live.wait_lingering(60), "the probe never reached its linger phase: %r" % live.lines)
        self.assertTrue(live.alive())
        record = mock.wait_record(0, timeout=2.0)
        self.assertTrue(record["client_closed_early"], record)
        self.assertTrue(live.alive(), "the probe exited before the close could be attributed to the request")

    def run_scenario(self, name: str, key: Optional[str] = None, args: Sequence[str] = (),
                     mock: Optional[Mock] = None, prompt: str = "hello") -> Tuple[ProbeRun, Mock]:
        mock = mock or self.start_mock()
        return self.probe(mock.url, "mock:" + name, key=key, args=args, prompt=prompt), mock

    def assert_clean_requests(self, mock: Mock) -> None:
        for record in mock.records():
            self.assertEqual(record["violations"], [], record)

    def assert_no_key(self, key: str, *texts: str) -> None:
        for text in texts:
            self.assertNotIn(key, text)

    # -- stream shapes -----------------------------------------------------------------------------

    def test_basic_stream_text_usage_and_timings(self) -> None:
        key = fake_key()
        run, mock = self.run_scenario("basic", key=key)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.text, "Test answer from the mock server.")
        done = run.done
        self.assertEqual(done["usage"], {"input": 25, "output": 12, "cache_read": 0, "cache_write": 0})
        self.assertEqual(done["stop_reason"], "end_turn")
        self.assertIsInstance(done["first_token_ms"], int)
        self.assertGreaterEqual(done["first_token_ms"], 0)
        self.assertGreaterEqual(done["total_ms"], done["first_token_ms"])
        self.assertEqual(run.last["event"], "done")
        # What the client sent: a valid request, the key only as the mock's digest.
        records = mock.records()
        self.assertEqual(len(records), 1)
        record = records[0]
        self.assertEqual(record["violations"], [])
        self.assertEqual(record["method"], "POST")
        self.assertEqual(record["path"], "/v1/messages")
        self.assertEqual(record["headers"]["authorization"], ml.key_digest("Bearer " + key))
        self.assertNotIn("x-api-key", record["headers"])
        self.assertEqual(record["headers"]["anthropic-version"], "2023-06-01")
        self.assertEqual(record["headers"]["content-type"], "application/json")
        self.assertEqual(record["headers"]["accept"], "text/event-stream")
        self.assertEqual(record["headers"]["accept-encoding"], "identity")
        self.assertTrue(record["headers"]["user-agent"].startswith("Curia/"))
        self.assertNotIn("expect", record["headers"])
        self.assertTrue(record["body_json"]["stream"])
        self.assertEqual(record["body_json"]["model"], "mock:basic")
        self.assertEqual(record["body_json"]["messages"][-1]["role"], "user")

    def test_stream_shapes_deliver_the_same_text(self) -> None:
        shapes = {
            "ping_interleaved": "Test answer from the mock server.",
            "unknown_event": "Test answer from the mock server.",
            "crlf": "Test answer from the mock server.",
            "basic_close_framing": "Test answer from the mock server.",
            "split_every_byte": "Test café costs €5 and \U0001F600 done.",
            "unicode_escaped": "Test café costs €5 and \U0001F600 done.",
        }
        mock = self.start_mock()
        for name, expected in shapes.items():
            with self.subTest(scenario=name):
                run, _ = self.run_scenario(name, mock=mock)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertEqual(run.text, expected)
                self.assertEqual(run.last["event"], "done")
                self.assertEqual(run.done["stop_reason"], "end_turn")
        self.assert_clean_requests(mock)

    def test_usage_positions(self) -> None:
        mock = self.start_mock()
        for name in ("usage_start_only", "usage_delta_only", "usage_both"):
            with self.subTest(scenario=name):
                run, _ = self.run_scenario(name, mock=mock)
                self.assertEqual(run.done["usage"],
                                 {"input": 21, "output": 15, "cache_read": 120, "cache_write": 340})

    def test_thinking_tokens_only_when_asked(self) -> None:
        mock = self.start_mock()
        plain, _ = self.run_scenario("thinking_blocks", mock=mock)
        self.assertEqual([e for e in plain.of("token") if e["kind"] == "thinking"], [])
        shown, _ = self.run_scenario("thinking_blocks", mock=mock, args=["--show-thinking"])
        thinking = [e["text"] for e in shown.of("token") if e["kind"] == "thinking"]
        self.assertTrue(thinking)
        self.assertEqual(shown.text, plain.text)

    def test_max_tokens_is_a_normal_end_and_refusal_is_policy(self) -> None:
        run, _ = self.run_scenario("max_tokens")
        self.assertEqual(run.returncode, 0)
        self.assertEqual(run.done["stop_reason"], "max_tokens")
        refused, _ = self.run_scenario("refusal")
        self.assertEqual(refused.returncode, 1)
        self.assertEqual(refused.error["code"], "Policy")
        self.assertFalse(refused.error["retryable"])
        self.assertEqual(refused.error["partial_len"], len("Test refusal."))

    def test_midstream_error_event_keeps_the_partial_text(self) -> None:
        run, _ = self.run_scenario("midstream_error")
        self.assertEqual(run.returncode, 1)
        error = run.error
        self.assertEqual(error["code"], "Overloaded")
        self.assertTrue(error["retryable"])
        self.assertEqual(error["http_status"], 0)
        self.assertEqual(run.text, "Test token 1.Test token 2.Test token 3.")
        self.assertEqual(error["partial_len"], len(run.text.encode("utf-8")))
        self.assertEqual(error["partial_text"], run.text)
        self.assertEqual(run.last["event"], "error")

    def test_truncated_and_reset_streams_are_network_errors_with_partial_text(self) -> None:
        mock = self.start_mock()
        for name, tokens in (("truncated", 2), ("reset_midstream", 3)):
            with self.subTest(scenario=name):
                run, _ = self.run_scenario(name, mock=mock)
                self.assertEqual(run.returncode, 1)
                self.assertEqual(run.error["code"], "Network")
                self.assertTrue(run.error["retryable"])
                expected = "".join("Test token %d." % n for n in range(1, tokens + 1))
                self.assertEqual(run.text, expected)
                self.assertEqual(run.error["partial_len"], len(expected))
                self.assertEqual(run.error["partial_text"], expected)

    def test_malformed_event_is_not_retryable(self) -> None:
        run, _ = self.run_scenario("bad_json")
        self.assertEqual(run.returncode, 1)
        self.assertEqual(run.error["code"], "Other")
        self.assertFalse(run.error["retryable"])

    # -- HTTP statuses -------------------------------------------------------------------------------

    def test_every_http_status_maps_to_the_right_error(self) -> None:
        table = [
            ("http_400", 400, "BadRequest", False),
            ("http_401", 401, "Auth", False),
            ("http_403", 403, "Auth", False),
            ("http_404", 404, "BadRequest", False),
            ("http_413", 413, "BadRequest", False),
            ("http_429_retry_after", 429, "RateLimit", True),
            ("http_500", 500, "Other", True),
            ("http_529", 529, "Overloaded", True),
        ]
        mock = self.start_mock()
        for scenario, status, code, retryable in table:
            with self.subTest(status=status):
                run, _ = self.run_scenario(scenario, mock=mock)
                self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
                error = run.error
                self.assertEqual(error["http_status"], status)
                self.assertEqual(error["code"], code)
                self.assertEqual(error["retryable"], retryable)
                self.assertEqual(error["partial_len"], 0)
                self.assertTrue(error["request_id"].startswith("req_mock"), error)
                self.assertIn("Mock ", error["message"])
                self.assertEqual(run.of("token"), [])
                expected_retry_after = 7 if status == 429 else None
                self.assertEqual(error["retry_after"], expected_retry_after)
        self.assert_clean_requests(mock)

    def test_connection_refused_is_a_retryable_network_error(self) -> None:
        run = self.probe("http://127.0.0.1:%d" % closed_port(), key=fake_key())
        self.assertEqual(run.returncode, 1)
        self.assertEqual(run.error["code"], "Network")
        self.assertTrue(run.error["retryable"])
        self.assertEqual(run.error["http_status"], 0)

    # -- stall and cancel --------------------------------------------------------------------------------

    def test_stall_guard_fires_and_does_not_hang(self) -> None:
        run, mock = self.run_scenario("stall", args=["--stall-timeout-ms", "1000"])
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        error = run.error
        self.assertEqual(error["code"], "Network")
        self.assertTrue(error["retryable"])
        self.assertIn("no data arrived", error["message"])
        self.assertEqual(run.text, "Test token 1.")
        self.assertEqual(error["partial_len"], len("Test token 1."))
        # The client's own clock: the configured second plus start-up, not libcurl's averaging window.
        self.assertLess(run.seconds, 6)
        # The connection was dropped (the mock's stall step would hold it for 120 s).
        self.assertTrue(mock.wait_record(0, timeout=10)["client_closed_early"])

    def test_cancel_after_tokens_closes_the_socket(self) -> None:
        for name in ("slow_forever", "hold_after_tokens"):
            with self.subTest(scenario=name):
                mock = self.start_mock()
                run, _ = self.run_scenario(name, mock=mock, args=["--cancel-after-tokens", "3"])
                self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
                error = run.error
                self.assertEqual(error["code"], "Cancelled")
                self.assertFalse(error["retryable"])
                tokens = run.of("token")
                self.assertGreaterEqual(len(tokens), 3)
                # The partial text is what was delivered before the cancel.
                self.assertEqual(error["partial_len"], len(run.text.encode("utf-8")))
                self.assertEqual(error["partial_text"], run.text)
                self.assertEqual(run.last["event"], "error")
                # A11 at the TCP level: the mock sees the client go, promptly, and sends no more.
                record = mock.wait_record(0, timeout=5)
                self.assertTrue(record["client_closed_early"])
                sent = record["events_sent"]
                time.sleep(0.4)
                self.assertEqual(mock.records()[0]["events_sent"], sent)
                self.assertLess(record["t_close"] - record["t_headers_done"], 10)

    def test_cancel_after_time_closes_the_socket(self) -> None:
        mock = self.start_mock()
        run, _ = self.run_scenario("slow_forever", mock=mock, args=["--cancel-after-ms", "1200"])
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertEqual(run.error["code"], "Cancelled")
        self.assertGreater(run.error["partial_len"], 0)
        self.assertEqual(run.error["partial_len"], len(run.text.encode("utf-8")))
        self.assertLess(run.seconds, 10)
        self.assertTrue(mock.wait_record(0, timeout=5)["client_closed_early"])

    def test_destroying_the_provider_mid_stream_calls_no_handler(self) -> None:
        mock = self.start_mock()
        run, _ = self.run_scenario("slow_forever", mock=mock, args=["--destroy-after-tokens", "2"])
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.last, {"event": "destroyed", "late_callbacks": 0})
        self.assertEqual(run.of("error"), [])
        self.assertEqual(run.of("done"), [])
        self.assertTrue(mock.wait_record(0, timeout=5)["client_closed_early"])

    def test_a_cancelled_request_closes_its_own_socket_before_the_process_exits(self) -> None:
        for name, flags in (("slow_forever", ["--cancel-after-tokens", "3"]),
                            ("hold_after_tokens", ["--cancel-after-tokens", "3"]),
                            ("slow_forever", ["--cancel-after-ms", "1000"])):
            with self.subTest(scenario=name, flags=flags):
                mock = self.start_mock()
                live = self.probe_live(mock, name, flags)
                self.assert_socket_closed_while_probe_lives(live, mock)

    def test_a_destroyed_provider_closes_its_socket_before_the_process_exits(self) -> None:
        mock = self.start_mock()
        live = self.probe_live(mock, "slow_forever", ["--destroy-after-tokens", "2"])
        self.assert_socket_closed_while_probe_lives(live, mock)

    def test_a_cancel_from_another_thread_is_reported_promptly(self) -> None:
        # The probe cancels from its main thread while the net thread idles in curl_multi_poll
        # (hold_after_tokens sends three tokens and then nothing). Without the token's waker the
        # cancel would wait for the net thread's one-second poll timeout (about 600 ms here).
        mock = self.start_mock()
        for attempt in range(3):
            run, _ = self.run_scenario("hold_after_tokens", mock=mock, args=["--cancel-after-ms", "400"])
            self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
            self.assertEqual(run.error["code"], "Cancelled")
            self.assertEqual(len(run.of("token")), 3)
            latency = run.error["total_ms"] - 400
            self.assertLess(latency, 300, "attempt %d: the cancel took %d ms to be reported" % (attempt, latency))

    # -- what is on the other end ---------------------------------------------------------------------

    def test_a_200_that_is_not_an_event_stream_is_said_so(self) -> None:
        scenario = pathlib.Path(self.tmp.name) / "portal.json"
        scenario.write_text(json.dumps({
            "name": "portal", "status": 200, "headers": {"content-type": "text/html"},
            "steps": [{"raw": "<html><body>Sign in to the hotspot</body></html>"}]}), encoding="utf-8")
        mock = self.start_mock("--scenario", str(scenario))
        run, _ = self.run_scenario("portal", mock=mock)
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        error = run.error
        self.assertEqual(error["code"], "Other")
        self.assertFalse(error["retryable"])
        self.assertEqual(error["http_status"], 200)
        self.assertIn("not with an event stream", error["message"])
        self.assertNotIn("hotspot", error["message"])
        self.assertEqual(run.of("token"), [])

    def test_something_that_does_not_speak_http_is_not_an_address_problem(self) -> None:
        server = RawServer(b"\x00\x01 this is not http at all\r\n\r\n")
        self.addCleanup(server.close)
        run = self.probe(server.url, key=fake_key())
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertEqual(run.error["code"], "Network")
        self.assertFalse(run.error["retryable"])
        self.assertNotIn("address", run.error["message"])
        self.assertIn("does not seem to be the API", run.error["message"])

    def test_a_server_that_never_answers_is_a_stall_not_a_connect_timeout(self) -> None:
        server = RawServer(b"", hold=True)
        self.addCleanup(server.close)
        run = self.probe(server.url, key=fake_key(), args=["--stall-timeout-ms", "1000"])
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertEqual(run.error["code"], "Network")
        self.assertTrue(run.error["retryable"])
        self.assertIn("no data arrived", run.error["message"])
        self.assertNotIn("connecting", run.error["message"])
        self.assertLess(run.seconds, 6)
        self.assertTrue(server.closed_early.wait(5))

    # -- secrecy -----------------------------------------------------------------------------------------

    def test_a_key_reflected_by_the_server_appears_nowhere(self) -> None:
        for name in ("echo_key", "echo_key_http"):
            with self.subTest(scenario=name):
                key = fake_key()
                mock = self.start_mock()
                run, _ = self.run_scenario(name, key=key, mock=mock, prompt="PROMPT-MARKER-4711")
                self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
                if name == "echo_key":
                    self.assertEqual(run.text, "The key is [redacted]")  # it was reflected, and scrubbed
                    self.assertEqual(run.error["code"], "Other")
                    self.assertIn("[redacted]", run.error["message"])
                    self.assertEqual(run.error["partial_text"], "The key is [redacted]")
                else:
                    self.assertEqual(run.error["code"], "Auth")
                    self.assertIn("[redacted]", run.error["message"])
                records_text = mock.records_text()
                output = mock.stop()
                self.assert_no_key(key, run.stdout, run.stderr, run.log_text(), records_text, output)
                # The mock saw the key only as its digest.
                self.assertIn(ml.key_digest("Bearer " + key), records_text)
                # The app log holds numbers, never the prompt or answer text.
                log = run.log_text()
                self.assertIn("net_done", log)
                self.assertNotIn("PROMPT-MARKER-4711", log)
                self.assertNotIn("The key is", log)
                self.assertNotIn("Bearer", log)

    def test_the_app_log_holds_only_numbers_and_short_names(self) -> None:
        run, _ = self.run_scenario("cache_demo", prompt="PROMPT-MARKER-0815",
                                   args=["--persona", "PERSONA-MARKER", "--snapshot", "gold: 77"])
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        log = run.log_text()
        for event in ("chat_request", "net_done", "chat_done"):
            self.assertIn(event, log)
        for text in ("PROMPT-MARKER-0815", "PERSONA-MARKER", "gold: 77", "Test answer", "mock:"):
            self.assertNotIn(text, log)
        self.assertNotIn("Bearer", log)
        self.assertNotIn("authorization", log.lower())

    def test_a_redirect_is_not_followed_and_the_key_stays_home(self) -> None:
        decoy = Listener()
        self.addCleanup(decoy.close)
        key = fake_key()
        mock = self.start_mock("--decoy-url", "http://127.0.0.1:%d/steal" % decoy.port)
        run, _ = self.run_scenario("redirect_307", key=key, mock=mock)
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertEqual(run.error["code"], "Network")
        self.assertFalse(run.error["retryable"])
        self.assertEqual(run.error["http_status"], 307)
        self.assertEqual(len(mock.records()), 1)
        time.sleep(0.3)  # a follow would have connected by now
        self.assertEqual(decoy.count(), 0)
        self.assertEqual(decoy.received, b"")
        self.assert_no_key(key, run.stdout, run.stderr, run.log_text())

    def test_a_proxy_in_the_environment_is_not_used_for_a_test_endpoint(self) -> None:
        # probe() sets every proxy variable to the tripwire; tearDown fails if it was ever contacted.
        run, mock = self.run_scenario("basic")
        self.assertEqual(run.returncode, 0)
        self.assertEqual(len(mock.records()), 1)

    def test_test_endpoints_must_be_loopback_http_literals(self) -> None:
        mock = self.start_mock()
        bad = [
            "http://192.0.2.1:80",
            "http://example.com:80",
            "http://localhost:%d" % mock.port,
            "https://127.0.0.1:%d" % mock.port,
            "http://127.0.0.1",
            "http://127.0.0.1:%d/other" % mock.port,
            "http://user@127.0.0.1:%d" % mock.port,
            "http://[::2]:%d" % mock.port,
            "http://10.0.0.1:%d" % mock.port,
        ]
        for endpoint in bad:
            with self.subTest(endpoint=endpoint):
                run = self.probe(endpoint, key=fake_key())
                self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
                self.assertEqual(run.error["code"], "Config")
                self.assertEqual(run.of("token"), [])
        self.assertEqual(mock.records(), [])

    def test_a_test_endpoint_needs_a_fake_key(self) -> None:
        mock = self.start_mock()
        key = real_looking_key()
        run = self.probe(mock.url, key=key)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertEqual(run.error["code"], "Config")
        self.assertIn("FAKE", run.error["message"])
        self.assert_no_key(key, run.stdout, run.stderr)
        self.assertEqual(mock.records(), [])

    def test_no_key_means_nothing_is_sent(self) -> None:
        mock = self.start_mock()
        run = self.probe(mock.url, env_key=False)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertEqual(run.error["code"], "Config")
        self.assertEqual(mock.records(), [])

    # -- auth styles, cache prefix ---------------------------------------------------------------------

    def test_x_api_key_fallback_sends_that_header_and_not_the_other(self) -> None:
        key = fake_key()
        mock = self.start_mock()
        bearer, _ = self.run_scenario("basic", key=key, mock=mock)
        legacy, _ = self.run_scenario("basic", key=key, mock=mock, args=["--auth", "x-api-key"])
        self.assertEqual(bearer.returncode, 0)
        self.assertEqual(legacy.returncode, 0)
        first, second = mock.records()
        self.assertEqual(first["headers"]["authorization"], ml.key_digest("Bearer " + key))
        self.assertNotIn("x-api-key", first["headers"])
        self.assertEqual(second["headers"]["x-api-key"], ml.key_digest(key))
        self.assertNotIn("authorization", second["headers"])
        self.assertEqual(second["violations"], [])
        self.assert_no_key(key, mock.records_text(), legacy.stdout, legacy.stderr, legacy.log_text())

    def test_bad_auth_style_is_a_usage_error(self) -> None:
        mock = self.start_mock()
        run = self.probe(mock.url, args=["--auth", "basic"])
        self.assertEqual(run.returncode, 2)
        self.assertEqual(mock.records(), [])

    def test_the_snapshot_reaches_the_request(self) -> None:
        run, mock = self.run_scenario("cite", args=["--persona", "You advise.",
                                                    "--snapshot", "gold: 321\nheir: Alys"])
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.text, "Gold is 321. Heir is Alys.")
        self.assert_clean_requests(mock)

    def test_request_bodies_are_identical_and_the_second_run_reads_the_cache(self) -> None:
        persona = "You are a careful advisor of a medieval realm. " * 100
        snapshot = "gold: 123\nheir: Alys\n" + ("realm detail line\n" * 120)
        args = ["--persona", persona, "--snapshot", snapshot]
        mock = self.start_mock()
        first, _ = self.run_scenario("cache_demo", mock=mock, args=args, key=fake_key())
        second, _ = self.run_scenario("cache_demo", mock=mock, args=args, key=fake_key())
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        one, two = mock.records()
        # Same bytes whatever the key: the prefix the cache keys on does not move.
        self.assertEqual(one["body_sha256"], two["body_sha256"])
        self.assertEqual(one["body_bytes_len"], two["body_bytes_len"])
        self.assertEqual(one["violations"], [])
        self.assertGreater(first.done["usage"]["cache_write"], 0)
        self.assertEqual(first.done["usage"]["cache_read"], 0)
        self.assertGreater(second.done["usage"]["cache_read"], 0)
        blocks = one["body_json"]["system"]
        self.assertEqual(len(blocks), 2)
        self.assertEqual(blocks[0]["cache_control"], {"type": "ephemeral"})
        self.assertEqual(blocks[1]["cache_control"], {"type": "ephemeral"})


def main() -> int:
    global PROBE, MOCK, ml
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--probe", required=True, help="path of curia_chat_probe")
    parser.add_argument("--mock", required=True, help="path of tools/mock_llm/mock_llm.py")
    args, rest = parser.parse_known_args()
    PROBE = str(pathlib.Path(args.probe).resolve())
    MOCK = str(pathlib.Path(args.mock).resolve())
    sys.path.insert(0, str(pathlib.Path(MOCK).parent))
    import mock_llm  # noqa: E402  (found through the path above)

    ml = mock_llm
    if not pathlib.Path(PROBE).exists():
        print("provider_mock: the probe does not exist: %s" % PROBE, file=sys.stderr)
        return 2
    program = unittest.main(argv=[sys.argv[0]] + rest, verbosity=2, exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    result = main()
    sys.exit(result)
