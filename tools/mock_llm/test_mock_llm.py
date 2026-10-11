# SPDX-License-Identifier: MIT
"""Unit tests for tools/mock_llm/mock_llm.py. Standard library only; synthetic data, loopback only.

Run: python3 -m unittest discover -s tools/mock_llm -v
The client is a raw-socket client written here (no urllib, so proxy environment variables cannot
interfere). Nothing waits on the wall clock for synchronisation: tests wait on conditions with
generous timeouts, the server listens on port 0 and the cache TTL test uses an injected clock.
"""
from __future__ import annotations

import base64
import copy
import hashlib
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from typing import Any, Callable, Dict, List, Optional, Tuple

import mock_llm as ml

HERE = os.path.dirname(os.path.abspath(__file__))
MOCK_PATH = os.path.join(HERE, "mock_llm.py")
FIXTURES = os.path.join(HERE, "..", "..", "tests", "fixtures", "anthropic")
TIMEOUT = 20.0
ANSWER = "Test answer from the mock server."
UTF8_ANSWER = "Test café costs €5 and \U0001F600 done."


def make_key() -> str:
    """A fake key built at run time: it contains FAKE and never appears in a tracked file."""
    return "sk-ant-" + "FAKE" + secrets.token_hex(10)


# --------------------------------------------------------------------------------------------
# A small raw-socket HTTP/1.1 client
# --------------------------------------------------------------------------------------------

class Reply:
    def __init__(self) -> None:
        self.status = 0
        self.headers: Dict[str, str] = {}
        self.interim: List[int] = []
        self.chunk_sizes: List[int] = []
        self.body = b""
        self.complete = False
        self.error: Optional[str] = None

    def json(self) -> Any:
        return json.loads(self.body.decode("utf-8"))


class Reader:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buf = b""

    def fill(self) -> None:
        data = self.sock.recv(65536)
        if not data:
            raise EOFError
        self.buf += data

    def until(self, delim: bytes) -> bytes:
        while delim not in self.buf:
            self.fill()
        head, self.buf = self.buf.split(delim, 1)
        return head

    def take(self, count: int) -> bytes:
        while len(self.buf) < count:
            self.fill()
        data, self.buf = self.buf[:count], self.buf[count:]
        return data


def read_reply(sock: socket.socket, stop_when: Optional[Callable[[bytes], bool]] = None) -> Reply:
    """Reads one response, decoding chunked bodies by hand. ``stop_when(body)`` ends the read early."""
    reply, reader = Reply(), Reader(sock)
    try:
        while True:
            head = reader.until(b"\r\n\r\n").decode("latin-1").split("\r\n")
            status = int(head[0].split(" ")[1])
            if 100 <= status < 200:
                reply.interim.append(status)
                continue
            break
        reply.status = status
        for line in head[1:]:
            name, _, value = line.partition(":")
            reply.headers[name.strip().lower()] = value.strip()
        if "chunked" in reply.headers.get("transfer-encoding", ""):
            while True:
                size = int(reader.until(b"\r\n"), 16)
                if size == 0:
                    reader.until(b"\r\n")
                    reply.complete = True
                    break
                reply.body += reader.take(size)
                if reader.take(2) != b"\r\n":
                    raise ValueError("chunk not followed by CRLF")
                reply.chunk_sizes.append(size)
                if stop_when and stop_when(reply.body):
                    break
        else:
            reply.body = reader.buf
            reader.buf = b""
            length = reply.headers.get("content-length")
            if length is not None:
                reply.body += reader.take(int(length) - len(reply.body))
                reply.complete = True
            else:
                while not (stop_when and stop_when(reply.body)):
                    try:
                        reader.fill()
                    except EOFError:
                        reply.complete = True
                        break
                    reply.body += reader.buf
                    reader.buf = b""
    except EOFError:
        reply.error = "eof"
    except (OSError, ValueError) as exc:
        reply.error = repr(exc)
    return reply


def valid_body(**changes: Any) -> Dict[str, Any]:
    body: Dict[str, Any] = {
        "model": ml.MODEL,
        "max_tokens": 64,
        "stream": True,
        "system": [{"type": "text", "text": "gold: 312\nheir: Test Heir Two",
                    "cache_control": {"type": "ephemeral"}}],
        "messages": [{"role": "user", "content": [{"type": "text", "text": "Is my heir safe?"}]}],
    }
    body.update(changes)
    return body


def request_bytes(method: str, path: str, headers: List[Tuple[str, str]], body: bytes) -> bytes:
    lines = ["%s %s HTTP/1.1" % (method, path)] + ["%s: %s" % h for h in headers]
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body


def api_headers(key: Optional[str], body: bytes, drop: Tuple[str, ...] = (),
                extra: Tuple[Tuple[str, str], ...] = ()) -> List[Tuple[str, str]]:
    headers = [("Host", "127.0.0.1"), ("Content-Type", "application/json"), ("anthropic-version", "2023-06-01"),
               ("Content-Length", str(len(body)))]
    if key is not None:
        headers.append(("x-api-key", key))
    headers = [h for h in headers if h[0].lower() not in drop]
    return headers + list(extra)


def connect(mock: Any) -> socket.socket:
    sock = socket.create_connection((mock.host, mock.port), timeout=TIMEOUT)
    sock.settimeout(TIMEOUT)
    return sock


def open_request(mock: Any, body: Any = None, *, key: Optional[str] = "FAKE-test-key", method: str = "POST",
                 path: str = "/v1/messages", raw: Optional[bytes] = None, drop: Tuple[str, ...] = (),
                 extra: Tuple[Tuple[str, str], ...] = ()) -> socket.socket:
    """Connects and sends a request (default: a valid Messages request); the caller reads the reply."""
    payload = raw if raw is not None else json.dumps(valid_body() if body is None else body).encode("utf-8")
    sock = connect(mock)
    sock.sendall(request_bytes(method, path, api_headers(key, payload, drop, extra), payload))
    return sock


def post(mock: Any, body: Any = None, stop_when: Optional[Callable[[bytes], bool]] = None,
         **kwargs: Any) -> Reply:
    sock = open_request(mock, body, **kwargs)
    try:
        return read_reply(sock, stop_when)
    finally:
        sock.close()


def get(mock: Any, path: str) -> Reply:
    sock = connect(mock)
    try:
        sock.sendall(request_bytes("GET", path, [("Host", "127.0.0.1")], b""))
        return read_reply(sock)
    finally:
        sock.close()


def parse_sse(data: bytes) -> List[Dict[str, Any]]:
    """A minimal SSE reader for assertions (LF or CRLF); ``json`` is None for a data line that is not JSON."""
    events = []
    for block in data.decode("utf-8").replace("\r\n", "\n").split("\n\n"):
        if not block.strip():
            continue
        event, lines = None, []
        for line in block.split("\n"):
            if line.startswith("event: "):
                event = line[len("event: "):]
            elif line.startswith("data: "):
                lines.append(line[len("data: "):])
        text = "\n".join(lines)
        try:
            parsed = json.loads(text)
        except ValueError:
            parsed = None
        events.append({"event": event, "data": text, "json": parsed})
    return events


def text_of(events: List[Dict[str, Any]]) -> str:
    out = []
    for e in events:
        j = e["json"]
        if isinstance(j, dict) and j.get("type") == "content_block_delta" and j["delta"].get("type") == "text_delta":
            out.append(j["delta"]["text"])
    return "".join(out)


def names_of(events: List[Dict[str, Any]]) -> List[str]:
    return [e["event"] for e in events]


def final_usage(events: List[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    for e in reversed(events):
        if isinstance(e["json"], dict) and e["json"].get("type") == "message_delta":
            return e["json"].get("usage")
    return None


def start_usage(events: List[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    return events[0]["json"]["message"].get("usage")


class FakeClock:
    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now


class MockCase(unittest.TestCase):
    def start(self, *scenarios: Any, **kwargs: Any) -> ml.MockLlm:
        mock = ml.MockLlm(list(scenarios) or None, **kwargs)
        mock.start()
        self.addCleanup(mock.stop)
        return mock


# --------------------------------------------------------------------------------------------
# Scenarios end to end
# --------------------------------------------------------------------------------------------

SSE_TEXT = {  # builtin -> the answer text its stream must carry
    "basic": ANSWER, "ping_interleaved": ANSWER, "unknown_event": ANSWER, "crlf": ANSWER,
    "thinking_blocks": ANSWER, "usage_start_only": ANSWER, "usage_delta_only": ANSWER, "usage_both": ANSWER,
    "basic_close_framing": ANSWER, "max_tokens": ANSWER, "refusal": "Test refusal.",
    "split_every_byte": UTF8_ANSWER, "unicode_escaped": UTF8_ANSWER,
}
HTTP_ERRORS = {  # builtin -> (status, error type)
    "http_400": (400, "invalid_request_error"), "http_401": (401, "authentication_error"),
    "http_403": (403, "permission_error"), "http_404": (404, "not_found_error"),
    "http_413": (413, "request_too_large"), "http_429_retry_after": (429, "rate_limit_error"),
    "http_500": (500, "api_error"), "http_529": (529, "overloaded_error"),
}
SPECIAL = {  # builtins with a test of their own below
    "midstream_error", "truncated", "bad_json", "stall", "slow_forever", "hold_after_tokens", "reset_midstream",
    "echo_key", "echo_key_http", "redirect_307", "cite", "cache_demo",
}


class ScenarioTests(MockCase):
    def test_every_builtin_scenario_is_covered_by_a_test(self):
        covered = set(SSE_TEXT) | set(HTTP_ERRORS) | SPECIAL
        self.assertEqual(covered, set(ml.builtin_names()))

    def test_success_streams(self):
        mock = self.start()
        for name, expected in sorted(SSE_TEXT.items()):
            with self.subTest(name):
                reply = post(mock, valid_body(model="mock:" + name))
                self.assertEqual(reply.status, 200)
                self.assertTrue(reply.complete, reply.error)
                self.assertTrue(reply.headers["content-type"].startswith("text/event-stream"))
                events = parse_sse(reply.body)
                self.assertEqual(text_of(events), expected)
                self.assertEqual(events[-1]["event"], "message_stop")
                self.assertEqual(reply.body, ml.wire_body(name))

    def test_http_error_scenarios(self):
        mock = self.start()
        for name, (status, kind) in sorted(HTTP_ERRORS.items()):
            with self.subTest(name):
                reply = post(mock, valid_body(model="mock:" + name))
                self.assertEqual(reply.status, status)
                self.assertTrue(reply.complete)
                body = reply.json()
                self.assertEqual(body["type"], "error")
                self.assertEqual(body["error"]["type"], kind)
                self.assertIsInstance(body["error"]["message"], str)
                self.assertTrue(reply.headers["request-id"].startswith("req_mock"))
        self.assertEqual(post(mock, valid_body(model="mock:http_429_retry_after")).headers["retry-after"], "7")

    def test_redirect_points_at_the_decoy_url(self):
        mock = self.start(decoy_url="http://127.0.0.1:1/decoy-test")
        reply = post(mock, valid_body(model="mock:redirect_307"))
        self.assertEqual(reply.status, 307)
        self.assertEqual(reply.headers["location"], "http://127.0.0.1:1/decoy-test")

    def test_midstream_error_arrives_after_http_200(self):
        reply = post(self.start("midstream_error"))
        events = parse_sse(reply.body)
        self.assertEqual(reply.status, 200)
        self.assertTrue(reply.complete)
        self.assertEqual(text_of(events), "Test token 1.Test token 2.Test token 3.")
        self.assertEqual(events[-1]["event"], "error")
        self.assertEqual(events[-1]["json"]["error"]["type"], "overloaded_error")
        self.assertNotIn("message_stop", names_of(events))

    def test_truncated_stream_has_no_message_stop_and_no_chunk_terminator(self):
        reply = post(self.start("truncated"))
        self.assertEqual(reply.status, 200)
        self.assertFalse(reply.complete)
        self.assertEqual(reply.error, "eof")
        self.assertNotIn("message_stop", names_of(parse_sse(reply.body)))
        self.assertEqual(reply.body, ml.wire_body("truncated"))

    def test_bad_json_line_is_sent_as_is(self):
        events = parse_sse(post(self.start("bad_json")).body)
        self.assertEqual([e["json"] is None for e in events].count(True), 1)
        self.assertEqual(events[-1]["event"], "message_stop")

    def test_thinking_blocks_carry_thinking_and_signature_deltas(self):
        events = parse_sse(post(self.start("thinking_blocks")).body)
        kinds = [e["json"]["delta"]["type"] for e in events if e["event"] == "content_block_delta"]
        self.assertEqual(kinds[:3], ["thinking_delta", "thinking_delta", "signature_delta"])
        self.assertEqual(kinds[3:], ["text_delta"] * 3)
        self.assertEqual(final_usage(events)["output_tokens"], 12)

    def test_stop_reasons(self):
        mock = self.start()
        for name, reason in (("refusal", "refusal"), ("max_tokens", "max_tokens"), ("basic", "end_turn")):
            events = parse_sse(post(mock, valid_body(model="mock:" + name)).body)
            stop = [e for e in events if e["event"] == "message_delta"][0]["json"]["delta"]["stop_reason"]
            self.assertEqual(stop, reason)

    def test_usage_placement(self):
        mock = self.start()
        full = {"input_tokens": 21, "cache_creation_input_tokens": 340, "cache_read_input_tokens": 120,
                "output_tokens": 15}
        cases = {
            "usage_start_only": (full, None),
            "usage_delta_only": (None, full),
            "usage_both": (dict(full, output_tokens=1), full),
        }
        for name, (at_start, at_delta) in cases.items():
            with self.subTest(name):
                events = parse_sse(post(mock, valid_body(model="mock:" + name)).body)
                self.assertEqual(start_usage(events), at_start)
                self.assertEqual(final_usage(events), at_delta)

    def test_unknown_event_and_ping_are_in_the_stream(self):
        mock = self.start()
        names = names_of(parse_sse(post(mock, valid_body(model="mock:unknown_event")).body))
        self.assertIn("mock_future_event", names)
        names = names_of(parse_sse(post(mock, valid_body(model="mock:ping_interleaved")).body))
        self.assertEqual(names.count("ping"), len(names) - names.count("ping") - 1)

    def test_crlf_stream_uses_crlf_only(self):
        body = post(self.start("crlf")).body
        self.assertNotIn(b"\n\n", body)
        self.assertEqual(body.count(b"\r\n"), body.count(b"\n"))
        self.assertEqual(text_of(parse_sse(body)), ANSWER)

    def test_unicode_escaped_stream_is_pure_ascii(self):
        body = post(self.start("unicode_escaped")).body
        body.decode("ascii")
        self.assertIn(b"\\u20ac", body)
        self.assertIn(b"\\ud83d\\ude00", body)

    def test_cite_quotes_the_snapshot(self):
        mock = self.start("cite")
        system = [{"type": "text", "text": "persona\nstuff"},
                  {"type": "text", "text": "frame\n  gold: 4711\nheir: Test Heir Two\nnoise"}]
        events = parse_sse(post(mock, valid_body(system=system)).body)
        self.assertEqual(text_of(events), "Gold is 4711. Heir is Test Heir Two.")
        events = parse_sse(post(mock, valid_body(system=[{"type": "text", "text": "nothing here"}])).body)
        self.assertEqual(text_of(events), "Gold is unknown. Heir is unknown.")

    def test_echo_key_reflects_the_received_key_into_the_response(self):
        mock = self.start("echo_key")
        key = make_key()
        events = parse_sse(post(mock, key=key).body)
        self.assertIn(key, text_of(events))
        self.assertEqual(events[-1]["event"], "error")
        self.assertIn(key, events[-1]["json"]["error"]["message"])
        reply = post(self.start("echo_key_http"), key=key)
        self.assertEqual(reply.status, 401)
        self.assertIn(key, reply.json()["error"]["message"])

    def test_echo_key_uses_the_bearer_token_without_its_prefix(self):
        mock = self.start("echo_key_http")
        key = make_key()
        reply = post(mock, key=None, extra=(("Authorization", "Bearer " + key),))
        self.assertIn(key, reply.json()["error"]["message"])
        self.assertNotIn("Bearer " + key, reply.json()["error"]["message"])


class FlowTests(MockCase):
    def test_reset_midstream_is_not_a_clean_end(self):
        mock = self.start("reset_midstream")
        reply = post(mock)
        self.assertFalse(reply.complete)
        self.assertNotIn("message_stop", names_of(parse_sse(reply.body)))
        # An RST, not a FIN: the client's recv fails with a reset (Windows may say "aborted"), never a plain EOF.
        self.assertIsNotNone(reply.error)
        self.assertNotEqual(reply.error, "eof")
        self.assertRegex(reply.error, "Reset|Aborted")
        self.assertTrue(mock.wait_closed(0, TIMEOUT))
        self.assertFalse(mock.requests[0]["client_closed_early"])

    def test_plain_hold_does_not_watch_the_client_but_until_client_close_does(self):
        scenario = {"name": "plain_hold", "steps": [{"raw": "a"}, {"hold_ms": 1500}, {"raw": "b"}]}
        mock = self.start(scenario)
        post(mock, stop_when=lambda body: body == b"a")  # the client closes during the hold
        self.assertTrue(mock.wait_closed(0, TIMEOUT))
        record = mock.requests[0]
        self.assertGreaterEqual(record["t_close"] - record["t_headers_done"], 1.4)  # it held for the full time
        watched = {"name": "watched_hold", "steps": [{"raw": "a"}, {"hold_ms": 60000, "until_client_close": True}]}
        mock = self.start(watched)
        post(mock, stop_when=lambda body: body == b"a")
        self.assertTrue(mock.wait_closed(0, 5.0))
        self.assertTrue(mock.requests[0]["client_closed_early"])

    def test_clean_close_step_ends_the_response_and_skips_the_rest(self):
        scenario = {"name": "early_end", "steps": [{"raw": "a"}, {"close": "clean"}, {"raw": "never sent"}]}
        reply = post(self.start(scenario))
        self.assertTrue(reply.complete)
        self.assertEqual(reply.body, b"a")

    def test_close_framing_has_no_length_and_no_chunking(self):
        reply = post(self.start("basic_close_framing"))
        self.assertNotIn("transfer-encoding", reply.headers)
        self.assertNotIn("content-length", reply.headers)
        self.assertEqual(reply.headers["connection"], "close")
        self.assertEqual(reply.body, ml.wire_body("basic"))
        self.assertEqual(reply.chunk_sizes, [])

    def test_hold_with_a_duration_ends_by_itself(self):
        scenario = {"name": "short_hold", "steps": [{"raw": "a"}, {"hold_ms": 30}, {"raw": "b"}]}
        reply = post(self.start(scenario))
        self.assertTrue(reply.complete)
        self.assertEqual(reply.body, b"ab")

    def test_loop_with_a_count(self):
        scenario = {"name": "looped", "steps": [{"loop": {"raw": "x", "delay_ms": 1}, "count": 3}]}
        self.assertEqual(post(self.start(scenario)).body, b"xxx")

    def test_slow_forever_streams_until_the_client_leaves(self):
        mock = self.start("slow_forever")
        sock = open_request(mock)
        try:
            reply = read_reply(sock, stop_when=lambda body: body.count(b"tok ") >= 3)
            self.assertEqual(reply.status, 200)
            self.assertGreaterEqual(reply.body.count(b"tok "), 3)
        finally:
            sock.close()
        self.assertTrue(mock.wait_closed(0, 2.0))
        record = mock.requests[0]
        self.assertTrue(record["client_closed_early"])
        self.assertGreaterEqual(record["events_sent"], 5)

    def _client_leaves(self, scenario: str, marker: bytes, reset: bool) -> Dict[str, Any]:
        mock = self.start(scenario)
        sock = open_request(mock)
        try:
            reply = read_reply(sock, stop_when=lambda body: marker in body)
            self.assertIn(marker, reply.body)
            if reset:  # close with SO_LINGER 0: the client sends an RST instead of a FIN
                import struct
                fmt = "HH" if sys.platform == "win32" else "ii"
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack(fmt, 1, 0))
        finally:
            sock.close()
        self.assertTrue(mock.wait_closed(0, 2.0), "the mock did not notice the client closing within 2 s")
        return mock.requests[0]

    def test_disconnect_while_holding_is_detected(self):
        for reset in (False, True):
            with self.subTest(reset=reset):
                record = self._client_leaves("hold_after_tokens", b"Test token 3.", reset)
                self.assertTrue(record["client_closed_early"])
                self.assertIsNotNone(record["t_close"])
                self.assertGreaterEqual(record["t_close"], record["t_headers_done"])

    def test_disconnect_while_stalled_is_detected(self):
        record = self._client_leaves("stall", b"Test token 1.", False)
        self.assertTrue(record["client_closed_early"])

    def test_a_finished_response_is_not_an_early_close(self):
        mock = self.start()
        post(mock)
        self.assertTrue(mock.wait_closed(0, TIMEOUT))
        record = mock.requests[0]
        self.assertFalse(record["client_closed_early"])
        self.assertEqual(record["events_sent"], 8)

    def test_two_clients_hold_independently(self):
        mock = self.start("hold_after_tokens")
        socks = []
        for number in range(2):  # one at a time, so that request 0 is the first connection
            socks.append(open_request(mock))
            self.assertTrue(mock.wait_for_requests(number + 1, TIMEOUT))
            read_reply(socks[number], stop_when=lambda body: b"Test token 3." in body)
        socks[0].close()
        self.assertTrue(mock.wait_closed(0, 2.0))
        self.assertIsNone(mock.requests[1]["t_close"])
        socks[1].close()
        self.assertTrue(mock.wait_closed(1, 2.0))

    def test_stop_does_not_hang_on_a_silent_client_or_a_held_response(self):
        mock = self.start("hold_after_tokens")
        silent = connect(mock)
        held = open_request(mock)
        read_reply(held, stop_when=lambda body: b"Test token 3." in body)
        done = threading.Event()
        threading.Thread(target=lambda: (mock.stop(), done.set()), daemon=True).start()
        self.assertTrue(done.wait(TIMEOUT))
        silent.close()
        held.close()


class WireTests(MockCase):
    def test_chunks_every_and_mid_utf8_split_the_http_chunks_exactly(self):
        scenario = {"name": "pieces", "steps": [
            {"raw": "0123456789", "chunks": [3, 3]},
            {"raw": "abcdef", "every": 2},
            {"text": "xéy", "split_mid_utf8": True, "piece_delay_ms": 5},
            {"b64": base64.b64encode(b"tail").decode()},
        ]}
        reply = post(self.start(scenario))
        self.assertTrue(reply.complete)
        self.assertEqual(reply.chunk_sizes, [3, 3, 4, 2, 2, 2, 2, 2, 4])
        self.assertEqual(reply.body, "0123456789abcdefxéytail".encode("utf-8"))

    def test_split_every_byte_is_one_http_chunk_per_byte(self):
        reply = post(self.start("split_every_byte"))
        data = ml.wire_body("split_every_byte")
        self.assertEqual(reply.body, data)
        self.assertEqual(reply.chunk_sizes, [1] * len(data))
        self.assertEqual(text_of(parse_sse(reply.body)), UTF8_ANSWER)

    def test_mid_utf8_split_really_cuts_inside_a_character(self):
        step = {"text": "café € \U0001F600", "split_mid_utf8": True}
        pieces = ml.split_pieces(ml.step_bytes(step), step)
        self.assertEqual(len(pieces), 2)
        with self.assertRaises(UnicodeDecodeError):
            pieces[0].decode("utf-8")
        self.assertEqual(pieces[0] + pieces[1], "café € \U0001F600".encode("utf-8"))

    def test_tcp_sends_of_split_pieces_can_be_observed(self):
        # The HTTP chunk framing above is exact; the TCP receive boundaries are best effort, so this
        # only requires that the paced pieces do not all arrive in one receive.
        scenario = {"name": "paced", "steps": [{"raw": "aaaa", "chunks": [1, 1, 1, 1], "piece_delay_ms": 100}]}
        mock = self.start(scenario)
        sock = open_request(mock)
        sizes = []
        try:
            data = b""
            while not data.endswith(b"0\r\n\r\n"):
                piece = sock.recv(65536)
                self.assertTrue(piece)
                sizes.append(len(piece))
                data += piece
        finally:
            sock.close()
        self.assertGreaterEqual(len(sizes), 2)

    def test_expect_100_continue(self):
        mock = self.start()
        payload = json.dumps(valid_body()).encode("utf-8")
        sock = connect(mock)
        try:
            headers = api_headers("FAKE-test-key", payload, extra=(("Expect", "100-continue"),))
            sock.sendall(request_bytes("POST", "/v1/messages", headers, b""))
            reader = Reader(sock)
            self.assertTrue(reader.until(b"\r\n\r\n").startswith(b"HTTP/1.1 100"))
            sock.sendall(payload)
            reply = read_reply(sock)
        finally:
            sock.close()
        self.assertEqual(reply.status, 200)
        self.assertEqual(text_of(parse_sse(reply.body)), ANSWER)
        self.assertTrue(mock.requests[0]["expect_100_sent"])
        self.assertEqual(mock.requests[0]["violations"], [])

    def test_the_client_sees_the_100_status_in_the_reply_reader(self):
        mock = self.start()
        payload = json.dumps(valid_body()).encode("utf-8")
        sock = connect(mock)
        try:
            headers = api_headers("FAKE-test-key", payload, extra=(("Expect", "100-continue"),))
            sock.sendall(request_bytes("POST", "/v1/messages", headers, payload))  # body sent at once
            reply = read_reply(sock)
        finally:
            sock.close()
        self.assertEqual(reply.interim, [100])
        self.assertEqual(reply.status, 200)

    def test_chunked_request_body_is_decoded(self):
        mock = self.start()
        payload = json.dumps(valid_body()).encode("utf-8")
        pieces = [payload[:10], payload[10:]]
        encoded = b"".join(b"%x\r\n%s\r\n" % (len(p), p) for p in pieces) + b"0\r\n\r\n"
        headers = [("Host", "127.0.0.1"), ("Content-Type", "application/json"),
                   ("anthropic-version", "2023-06-01"), ("Transfer-Encoding", "chunked")]
        sock = connect(mock)
        try:
            sock.sendall(request_bytes("POST", "/v1/messages", headers, encoded))
            reply = read_reply(sock)
        finally:
            sock.close()
        self.assertEqual(reply.status, 200)
        record = mock.requests[0]
        self.assertEqual(record["body_json"]["model"], ml.MODEL)
        self.assertEqual(record["body_sha256"], hashlib.sha256(payload).hexdigest())

    def test_bad_request_line_gets_a_400(self):
        mock = self.start()
        sock = connect(mock)
        try:
            sock.sendall(b"NONSENSE\r\n\r\n")
            reply = read_reply(sock)
        finally:
            sock.close()
        self.assertEqual(reply.status, 400)
        self.assertEqual(mock.requests, [])

    def exchange(self, mock: ml.MockLlm, data: bytes) -> Reply:
        sock = connect(mock)
        try:
            sock.sendall(data)
            return read_reply(sock)
        finally:
            sock.close()

    def chunked_head(self) -> bytes:
        return (b"POST /v1/messages HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                b"anthropic-version: 2023-06-01\r\nTransfer-Encoding: chunked\r\n\r\n")

    def test_oversize_bodies_get_a_413_before_the_body_is_read(self):
        mock = self.start()
        self.assertEqual(ml.MAX_BODY_BYTES, 32 * 1024 * 1024)
        head = request_bytes("POST", "/v1/messages", [("Host", "127.0.0.1"), ("Content-Length", "33554433")], b"")
        self.assertEqual(self.exchange(mock, head).status, 413)  # no body is ever sent
        reply = self.exchange(mock, self.chunked_head() + b"FFFFFFFFFF\r\n")  # a 1 TB chunk, announced only
        self.assertEqual(reply.status, 413)
        self.assertEqual(mock.requests, [])

    def test_strict_request_framing(self):
        mock = self.start()
        payload = json.dumps(valid_body()).encode("utf-8")
        good = api_headers("FAKE-test-key", payload)
        two = good + [("Content-Length", "3")]
        self.assertEqual(self.exchange(mock, request_bytes("POST", "/v1/messages", two, payload)).status, 400)
        same = good + [("Content-Length", str(len(payload)))]
        self.assertEqual(self.exchange(mock, request_bytes("POST", "/v1/messages", same, payload)).status, 200)
        for bad in ("+5", "0x10", "1_0", "-1", "ten"):
            with self.subTest(content_length=bad):
                headers = [h for h in good if h[0] != "Content-Length"] + [("Content-Length", bad)]
                self.assertEqual(self.exchange(mock, request_bytes("POST", "/v1/messages", headers, b"")).status, 400)
        for size in (b"0x10", b"+5", b"1_0", b""):
            with self.subTest(chunk_size=size):
                self.assertEqual(self.exchange(mock, self.chunked_head() + size + b"\r\nabc\r\n0\r\n\r\n").status, 400)
        wrong_end = self.chunked_head() + b"3\r\nabcXX0\r\n\r\n"
        self.assertEqual(self.exchange(mock, wrong_end).status, 400)

    def test_only_post_messages_plays_a_scenario(self):
        mock = self.start("http_500", "basic")
        models = get(mock, "/v1/models")
        self.assertEqual(models.status, 200)
        self.assertTrue(models.headers["content-type"].startswith("application/json"))
        self.assertEqual([m["id"] for m in models.json()["data"]], [ml.MODEL])
        head = self.exchange(mock, request_bytes("HEAD", "/v1/messages", [("Host", "127.0.0.1")], b""))
        self.assertEqual((head.status, head.body), (405, b""))
        self.assertEqual(get(mock, "/v1/messages").status, 405)
        delete = self.exchange(mock, request_bytes("DELETE", "/v1/messages", [("Host", "127.0.0.1")], b""))
        self.assertEqual(delete.status, 405)
        self.assertEqual(delete.headers["allow"], "POST")
        self.assertEqual(delete.json()["error"]["type"], "invalid_request_error")
        unknown = get(mock, "/v1/nothing")
        self.assertEqual(unknown.status, 404)
        self.assertEqual(unknown.json()["error"]["type"], "not_found_error")
        self.assertEqual(self.exchange(mock, request_bytes("HEAD", "/v1/models", [("Host", "127.0.0.1")], b"")).body, b"")
        # None of that consumed the queue: the first scenario is still the first answer.
        self.assertEqual(post(mock).status, 500)
        self.assertEqual(post(mock).status, 200)
        self.assertEqual(post(mock, path="/v1/messages?beta=1").status, 200)


# --------------------------------------------------------------------------------------------
# Selection
# --------------------------------------------------------------------------------------------

class SelectionTests(MockCase):
    def test_queue_is_consumed_one_per_request_and_the_last_repeats(self):
        mock = self.start("http_500", "http_529", "basic")
        statuses = [post(mock).status for _ in range(5)]
        self.assertEqual(statuses, [500, 529, 200, 200, 200])
        self.assertEqual([r["scenario"] for r in mock.requests], ["http_500", "http_529", "basic", "basic", "basic"])

    def test_a_single_scenario_repeats(self):
        mock = self.start("http_400")
        self.assertEqual([post(mock).status for _ in range(3)], [400, 400, 400])

    def test_empty_queue_uses_the_default(self):
        mock = self.start(default="http_403")
        self.assertEqual(post(mock).status, 403)

    def test_mock_model_prefix_selects_a_builtin_without_consuming_the_queue(self):
        mock = self.start("http_500", "basic")
        self.assertEqual(post(mock, valid_body(model="mock:http_404")).status, 404)
        self.assertEqual(post(mock).status, 500)
        self.assertEqual(post(mock, valid_body(model="mock:http_404")).status, 404)
        self.assertEqual(post(mock).status, 200)

    def test_unknown_mock_name_is_a_404_in_the_anthropic_shape(self):
        mock = self.start()
        reply = post(mock, valid_body(model="mock:no_such_scenario"))
        self.assertEqual(reply.status, 404)
        body = reply.json()
        self.assertEqual(body["type"], "error")
        self.assertEqual(body["error"]["type"], "not_found_error")
        self.assertIn("no_such_scenario", body["error"]["message"])
        self.assertEqual(mock.requests[0]["scenario"], "unknown:no_such_scenario")

    def test_custom_scenarios_can_be_added_and_selected_by_name(self):
        mock = self.start()
        mock.add_scenario({"name": "mine", "status": 418, "steps": [{"text": {"hello": "world"}}]})
        self.assertEqual(post(mock, valid_body(model="mock:mine")).json(), {"hello": "world"})
        self.assertEqual(post(mock).status, 418)  # it is in the queue too
        mock.set_scenarios(["basic"])
        self.assertEqual(post(mock).status, 200)

    def test_scenario_files(self):
        path = os.path.join(HERE, "scenarios", "example.json")
        mock = self.start(path)
        reply = post(mock)
        self.assertEqual(text_of(parse_sse(reply.body)), "Test ruler one says hello.")
        self.assertEqual(reply.headers["request-id"], "req_example0001")
        cut = post(mock, valid_body(model="mock:example_cut"))
        self.assertFalse(cut.complete)
        self.assertIn(b"Test cut", cut.body)
        self.assertEqual(cut.chunk_sizes[:1], [len(b"event: ping\ndata: {\"type\":\"ping\"}\n\n")])

    def test_malformed_scenarios_are_refused_when_added(self):
        bad = [
            {"steps": [{"raw": "x", "bogus": 1}]},
            {"steps": [{"nothing": 1}]},
            {"steps": [{"raw": "x", "sse": {"data": 1}}]},
            {"status": 99},
            {"framing": "other"},
            {"steps": [{"close": "maybe"}]},
            {"steps": [{"raw": "x", "chunks": [0]}]},
            {"steps": [{"gen": "other"}]},
            {"steps": [{"b64": "not base64!"}]},
            "no_such_scenario_or_file",
        ]
        mock = ml.MockLlm()
        for scenario in bad:
            with self.subTest(scenario=scenario):
                with self.assertRaises(ValueError):
                    mock.add_scenario(scenario)

    def test_require_auth(self):
        mock = self.start(require_auth=True)
        missing = post(mock, key=None)
        self.assertEqual(missing.status, 401)
        self.assertEqual(missing.json()["error"]["type"], "authentication_error")
        self.assertEqual(post(mock, key="FAKE-test-key").status, 200)
        self.assertEqual(post(mock, key=None, extra=(("Authorization", "Bearer FAKE-test-key"),)).status, 200)
        # Off by default.
        self.assertEqual(post(self.start(), key=None).status, 200)


# --------------------------------------------------------------------------------------------
# Cache emulator
# --------------------------------------------------------------------------------------------

def big_system(marker: str, size: int = 3000) -> List[Dict[str, Any]]:
    text = (marker + " ") * (size // (len(marker) + 1))
    return [{"type": "text", "text": text, "cache_control": {"type": "ephemeral"}}]


def user(text: str) -> List[Dict[str, Any]]:
    return [{"role": "user", "content": [{"type": "text", "text": text}]}]


class CacheTests(MockCase):
    def usage_of_request(self, mock: ml.MockLlm, **changes: Any) -> Dict[str, Any]:
        events = parse_sse(post(mock, valid_body(model="mock:cache_demo", **changes)).body)
        usage = final_usage(events)
        self.assertEqual(start_usage(events)["input_tokens"], usage["input_tokens"])
        return usage

    def test_first_request_creates_and_the_second_reads(self):
        mock = self.start()
        first = self.usage_of_request(mock, system=big_system("persona"))
        second = self.usage_of_request(mock, system=big_system("persona"))
        self.assertGreater(first["cache_creation_input_tokens"], 512)
        self.assertEqual(first["cache_read_input_tokens"], 0)
        self.assertEqual(second["cache_creation_input_tokens"], 0)
        self.assertEqual(second["cache_read_input_tokens"], first["cache_creation_input_tokens"])
        self.assertEqual(first["input_tokens"], second["input_tokens"])  # only the tokens after the breakpoint
        self.assertLess(first["input_tokens"], 20)
        self.assertEqual(first["output_tokens"], 12)
        self.assertEqual(mock.requests[1]["cache"]["cache_read_input_tokens"], second["cache_read_input_tokens"])

    def test_text_after_the_last_breakpoint_does_not_break_the_cache(self):
        mock = self.start()
        first = self.usage_of_request(mock, system=big_system("persona"), messages=user("first question"))
        second = self.usage_of_request(mock, system=big_system("persona"),
                                       messages=user("a second and much longer question than the first"))
        self.assertEqual(second["cache_read_input_tokens"], first["cache_creation_input_tokens"])
        self.assertGreater(second["input_tokens"], first["input_tokens"])

    def test_a_changed_prefix_is_a_new_entry(self):
        mock = self.start()
        self.usage_of_request(mock, system=big_system("persona"))
        other = self.usage_of_request(mock, system=big_system("different"))
        self.assertEqual(other["cache_read_input_tokens"], 0)
        self.assertGreater(other["cache_creation_input_tokens"], 512)

    def test_below_the_minimum_nothing_is_cached(self):
        mock = self.start()
        small = [{"type": "text", "text": "short persona", "cache_control": {"type": "ephemeral"}}]
        for _ in range(2):
            usage = self.usage_of_request(mock, system=small)
            self.assertEqual(usage["cache_creation_input_tokens"], 0)
            self.assertEqual(usage["cache_read_input_tokens"], 0)
            self.assertGreater(usage["input_tokens"], 0)

    def test_minimum_is_configurable(self):
        mock = self.start(min_cacheable_tokens=4)
        small = [{"type": "text", "text": "short persona text", "cache_control": {"type": "ephemeral"}}]
        first = self.usage_of_request(mock, system=small)
        second = self.usage_of_request(mock, system=small)
        self.assertGreater(first["cache_creation_input_tokens"], 0)
        self.assertEqual(second["cache_read_input_tokens"], first["cache_creation_input_tokens"])

    def test_no_breakpoint_means_everything_is_input(self):
        mock = self.start()
        plain = [{"type": "text", "text": "persona " * 400}]
        usage = self.usage_of_request(mock, system=plain)
        self.assertEqual(usage["cache_creation_input_tokens"], 0)
        self.assertEqual(usage["cache_read_input_tokens"], 0)
        self.assertGreater(usage["input_tokens"], 512)

    def test_two_breakpoints_reuse_the_stable_part(self):
        mock = self.start()
        persona = big_system("persona")
        snapshot_one = [{"type": "text", "text": "snapshot one", "cache_control": {"type": "ephemeral"}}]
        snapshot_two = [{"type": "text", "text": "snapshot two!", "cache_control": {"type": "ephemeral"}}]
        first = self.usage_of_request(mock, system=persona + snapshot_one)
        second = self.usage_of_request(mock, system=persona + snapshot_two)
        # The persona prefix is read; only the changed snapshot is written.
        self.assertGreater(second["cache_read_input_tokens"], 512)
        self.assertLess(second["cache_read_input_tokens"], first["cache_creation_input_tokens"])
        self.assertGreaterEqual(second["cache_creation_input_tokens"], 1)
        third = self.usage_of_request(mock, system=persona + snapshot_two)
        self.assertEqual(third["cache_creation_input_tokens"], 0)
        self.assertEqual(third["cache_read_input_tokens"],
                         second["cache_read_input_tokens"] + second["cache_creation_input_tokens"])

    def test_ttl_expiry_and_refresh_with_an_injected_clock(self):
        clock = FakeClock()
        cache = ml.CacheEmulator(min_tokens=10, ttl=300.0, clock=clock)
        body = valid_body(system=big_system("persona", 400))
        first = cache.usage(body)
        self.assertGreater(first["cache_creation_input_tokens"], 0)
        clock.now += 299
        self.assertEqual(cache.usage(body)["cache_read_input_tokens"], first["cache_creation_input_tokens"])
        clock.now += 299  # the read refreshed the entry
        self.assertGreater(cache.usage(body)["cache_read_input_tokens"], 0)
        clock.now += 301  # past the TTL: gone
        again = cache.usage(body)
        self.assertEqual(again["cache_read_input_tokens"], 0)
        self.assertEqual(again["cache_creation_input_tokens"], first["cache_creation_input_tokens"])

    def test_one_hour_ttl_marker_lives_longer(self):
        clock = FakeClock()
        cache = ml.CacheEmulator(min_tokens=10, ttl=300.0, clock=clock)
        system = big_system("persona", 400)
        system[0]["cache_control"] = {"type": "ephemeral", "ttl": "1h"}
        body = valid_body(system=system)
        cache.usage(body)
        clock.now += 3000
        self.assertGreater(cache.usage(body)["cache_read_input_tokens"], 0)

    def test_ttl_expiry_through_the_server_uses_the_injected_clock(self):
        clock = FakeClock()
        mock = self.start(clock=clock, cache_ttl=5.0)
        self.usage_of_request(mock, system=big_system("persona"))
        clock.now += 6
        usage = self.usage_of_request(mock, system=big_system("persona"))
        self.assertEqual(usage["cache_read_input_tokens"], 0)
        self.assertGreater(usage["cache_creation_input_tokens"], 0)

    def test_tools_come_first_in_the_prefix_and_count(self):
        cache = ml.CacheEmulator(min_tokens=10)
        tool = {"name": "mock_tool", "description": "x" * 400, "input_schema": {"type": "object"},
                "cache_control": {"type": "ephemeral"}}
        body = valid_body(tools=[tool], system=[{"type": "text", "text": "persona"}])
        first = cache.usage(body)
        self.assertGreater(first["cache_creation_input_tokens"], 90)
        self.assertGreater(cache.usage(body)["cache_read_input_tokens"], 90)


# --------------------------------------------------------------------------------------------
# Request validation and records
# --------------------------------------------------------------------------------------------

class ValidationTests(MockCase):
    def violations(self, body: Any = None, status: int = 200, **kwargs: Any) -> List[str]:
        mock = self.start()
        reply = post(mock, body, **kwargs)
        self.assertEqual(reply.status, status, "violations never reject a routable request")
        self.assertTrue(reply.complete)
        return mock.requests[0]["violations"]

    def test_a_valid_request_has_no_violations(self):
        self.assertEqual(self.violations(), [])

    def test_header_and_framing_rules(self):
        self.assertIn("missing_anthropic_version", self.violations(drop=("anthropic-version",)))
        self.assertIn("missing_content_type", self.violations(drop=("content-type",)))
        self.assertIn("bad_content_type", self.violations(extra=(("content-type", "text/plain"),),
                                                            drop=("content-type",)))
        self.assertIn("wrong_path", self.violations(path="/v1/other", status=404))
        self.assertIn("wrong_method", self.violations(method="PUT", status=405))
        self.assertIn("body_not_json", self.violations(raw=b"not json"))
        self.assertIn("body_not_object", self.violations(raw=b"[1, 2]"))
        self.assertEqual(self.violations(path="/v1/messages?beta=1"), [])

    def test_body_rules(self):
        cases = [
            ("stream_not_true", valid_body(stream=False)),
            ("stream_not_true", {k: v for k, v in valid_body().items() if k != "stream"}),
            ("system_not_text_blocks", valid_body(system="a plain string")),
            ("system_not_text_blocks", valid_body(system=[{"type": "image"}])),
            ("bad_cache_control", valid_body(system=[{"type": "text", "text": "x",
                                                      "cache_control": {"type": "persistent"}}])),
            ("bad_cache_control", valid_body(system=[{"type": "text", "text": "x",
                                                      "cache_control": {"type": "ephemeral", "ttl": "5m"}}])),
            ("bad_cache_control", valid_body(system=[{"type": "text", "text": "x", "cache_control": "ephemeral"}])),
            ("too_many_cache_breakpoints", valid_body(system=[
                {"type": "text", "text": "x%d" % n, "cache_control": {"type": "ephemeral"}} for n in range(5)])),
            ("messages_not_starting_with_user", valid_body(messages=[
                {"role": "assistant", "content": [{"type": "text", "text": "hi"}]}])),
            ("messages_not_alternating", valid_body(messages=user("a") + user("b"))),
            ("bad_role", valid_body(messages=[{"role": "system", "content": "x"}])),
            ("missing_messages", valid_body(messages=[])),
            ("empty_text_block", valid_body(messages=user(""))),
            ("empty_text_block", valid_body(messages=[{"role": "user", "content": "  "}])),
            ("empty_text_block", valid_body(system=[{"type": "text", "text": ""}])),
            ("missing_model", {k: v for k, v in valid_body().items() if k != "model"}),
            ("missing_max_tokens", {k: v for k, v in valid_body().items() if k != "max_tokens"}),
            ("missing_max_tokens", valid_body(max_tokens="64")),
        ]
        for code, body in cases:
            with self.subTest(code=code):
                self.assertIn(code, self.violations(body))

    def test_forbidden_keys_are_found_anywhere(self):
        names = ("budget_tokens", "thinking", "temperature", "top_p", "top_k", "tool_choice")
        self.assertEqual(tuple(ml.FORBIDDEN_KEYS), names)  # the contract: these six, no more, no less
        for key in names:
            with self.subTest(key=key):
                self.assertIn("forbidden_key:" + key, self.violations(valid_body(**{key: 1})))
        nested = valid_body(messages=[{"role": "user", "content": [{"type": "text", "text": "x"}],
                                       "meta": {"deep": [{"temperature": 0.5}]}}])
        self.assertIn("forbidden_key:temperature", self.violations(nested))

    def test_valid_one_hour_ttl_and_two_breakpoints_are_fine(self):
        body = valid_body(system=[
            {"type": "text", "text": "persona", "cache_control": {"type": "ephemeral", "ttl": "1h"}},
            {"type": "text", "text": "snapshot", "cache_control": {"type": "ephemeral"}}])
        self.assertEqual(self.violations(body), [])

    def test_records_have_the_documented_fields(self):
        mock = self.start()
        payload = json.dumps(valid_body()).encode("utf-8")
        post(mock, raw=payload, extra=(("X-Test", "one"), ("X-Test", "two")))
        self.assertTrue(mock.wait_closed(0, TIMEOUT))
        record = mock.requests[0]
        for field in ("method", "path", "headers", "body_json", "body_bytes_len", "body_sha256", "scenario",
                      "t_accept", "t_headers_done", "t_close", "client_closed_early", "events_sent", "violations"):
            self.assertIn(field, record)
        self.assertEqual(record["method"], "POST")
        self.assertEqual(record["path"], "/v1/messages")
        self.assertEqual(record["headers"]["x-test"], ["one", "two"])
        self.assertEqual(record["headers"]["anthropic-version"], "2023-06-01")
        self.assertEqual(record["body_bytes_len"], len(payload))
        self.assertEqual(record["body_sha256"], hashlib.sha256(payload).hexdigest())
        self.assertEqual(record["body_json"]["model"], ml.MODEL)
        self.assertLessEqual(record["t_accept"], record["t_headers_done"])
        self.assertLessEqual(record["t_headers_done"], record["t_close"])
        self.assertEqual(record["scenario"], "basic")

    def test_requests_endpoint_matches_the_library_view(self):
        mock = self.start()
        post(mock)
        self.assertTrue(mock.wait_closed(0, TIMEOUT))
        reply = get(mock, "/__mock/requests")
        self.assertEqual(reply.status, 200)
        self.assertEqual(reply.json(), mock.requests)  # and the endpoint itself is not recorded
        self.assertEqual(len(mock.requests), 1)

    def test_unknown_control_path_is_404(self):
        self.assertEqual(get(self.start(), "/__mock/nothing").status, 404)

    def test_wait_helpers_time_out_cleanly(self):
        mock = self.start()
        self.assertFalse(mock.wait_for_requests(1, 0.05))
        self.assertFalse(mock.wait_closed(0, 0.05))
        post(mock)
        self.assertTrue(mock.wait_for_requests(1, TIMEOUT))


# --------------------------------------------------------------------------------------------
# Key safety
# --------------------------------------------------------------------------------------------

class KeySafetyTests(MockCase):
    def test_key_digest_shape(self):
        digest = ml.key_digest("abcdefgh")
        self.assertRegex(digest, r"^sha256:[0-9a-f]{12}\(len=8\)$")
        self.assertEqual(digest, ml.key_digest(b"abcdefgh"))
        self.assertEqual(digest, "sha256:%s(len=8)" % hashlib.sha256(b"abcdefgh").hexdigest()[:12])

    def send_with_keys(self, mock: ml.MockLlm, key_a: str, key_b: str) -> Reply:
        body = valid_body(messages=user("my key is " + key_a + " and the bearer token is " + key_b))
        return post(mock, body, key=key_a, path="/v1/messages?k=%s&t=%s" % (key_a, key_b),
                    extra=(("Authorization", "Bearer " + key_b), ("Proxy-Authorization", "Basic " + key_b)))

    def test_the_clear_key_is_nowhere_in_records_or_endpoint_or_files(self):
        key_a, key_b = make_key(), make_key()
        with tempfile.TemporaryDirectory() as directory:
            mock = self.start("echo_key", record_dir=directory)
            reply = self.send_with_keys(mock, key_a, key_b)
            self.assertIn(key_a, reply.body.decode("utf-8"))  # echo_key reflects it into the response
            self.assertTrue(mock.wait_closed(0, TIMEOUT))
            record = mock.requests[0]
            dumped = json.dumps(record)
            endpoint = get(mock, "/__mock/requests").body.decode("utf-8")
            files = sorted(os.listdir(directory))
            self.assertEqual(files, ["request-0000.json"])
            with open(os.path.join(directory, files[0]), "rb") as handle:
                on_disk = handle.read().decode("utf-8")
            for text in (dumped, endpoint, on_disk):
                self.assertNotIn(key_a, text)
                self.assertNotIn(key_b, text)
            self.assertEqual(record["headers"]["x-api-key"], ml.key_digest(key_a))
            self.assertEqual(record["headers"]["authorization"], ml.key_digest("Bearer " + key_b))
            self.assertEqual(record["headers"]["proxy-authorization"], ml.key_digest("Basic " + key_b))
            self.assertIn(ml.key_digest(key_a), record["path"])
            self.assertIn(ml.key_digest(key_a), record["body_json"]["messages"][0]["content"][0]["text"])
            # The bare token after "Bearer " is masked on its own too, in the body and in the path.
            self.assertIn(ml.key_digest(key_b), record["body_json"]["messages"][0]["content"][0]["text"])
            self.assertIn(ml.key_digest(key_b), record["path"])
            self.assertEqual(json.loads(on_disk)["headers"]["x-api-key"], ml.key_digest(key_a))

    def test_a_key_with_json_special_characters_is_scrubbed_in_both_spellings(self):
        key = make_key() + '"q\\z'
        escaped = json.dumps(key)[1:-1]
        self.assertNotEqual(key, escaped)
        mock = self.start()
        post(mock, valid_body(messages=user("key " + key)), key=key)
        self.assertEqual(mock.scrub("x " + key + " y"), "x " + ml.key_digest(key) + " y")
        self.assertEqual(mock.scrub("x " + escaped + " y"), "x " + ml.key_digest(key) + " y")
        for text in (json.dumps(mock.requests), get(mock, "/__mock/requests").body.decode("utf-8")):
            self.assertNotIn(key, text)
            self.assertNotIn(escaped, text)

    def test_validation_and_cache_see_the_real_body_but_the_record_shows_the_digest(self):
        key = make_key()
        mock = self.start("cache_demo")
        post(mock, valid_body(messages=user("key " + key)), key=key)
        self.assertEqual(mock.requests[0]["violations"], [])
        self.assertNotIn(key, json.dumps(mock.requests))

    def test_cli_never_prints_the_key(self):
        key = make_key()
        with tempfile.TemporaryDirectory() as directory:
            proc, port = start_cli(["--scenario", "echo_key", "--verbose", "--record-dir", directory])
            try:
                mock_like = Addr(port)
                reply = self.send_with_keys(mock_like, key, make_key())
                self.assertIn(key, reply.body.decode("utf-8"))
                self.assertEqual(post(mock_like, valid_body(model="mock:echo_key_http"), key=key).status, 401)
                dump = get(mock_like, "/__mock/requests").body.decode("utf-8")
                self.assertNotIn(key, dump)
                self.assertEqual(get(mock_like, "/__mock/nothing").status, 404)
                quit_cli(mock_like)
                out, err = finish_cli(proc)
            finally:
                if proc.poll() is None:
                    proc.kill()
            self.assertTrue(err, "--verbose should have logged something to stderr")
            self.assertNotIn(key.encode(), out)
            self.assertNotIn(key.encode(), err)
            for name in os.listdir(directory):
                with open(os.path.join(directory, name), "rb") as handle:
                    self.assertNotIn(key.encode(), handle.read())


# --------------------------------------------------------------------------------------------
# Loopback only
# --------------------------------------------------------------------------------------------

class LoopbackTests(MockCase):
    def test_library_refuses_other_hosts(self):
        for host in ("0.0.0.0", "localhost", "192.168.1.5", "::", "example.com", ""):
            with self.subTest(host=host):
                with self.assertRaises(ValueError):
                    ml.MockLlm(host=host)

    def test_the_socket_is_bound_to_loopback_on_an_os_chosen_port(self):
        mock = self.start()
        self.assertTrue(mock.url.startswith("http://127.0.0.1:"))
        self.assertGreater(mock.port, 0)
        probe = connect(mock)
        try:
            self.assertEqual(probe.getpeername()[0], "127.0.0.1")
        finally:
            probe.close()

    def test_two_servers_get_different_ports(self):
        self.assertNotEqual(self.start().port, self.start().port)

    def test_ipv6_loopback_when_available(self):
        try:
            mock = ml.MockLlm(host="::1")
            mock.start()
        except OSError:
            self.skipTest("no IPv6 loopback on this machine")
        self.addCleanup(mock.stop)
        self.assertTrue(mock.url.startswith("http://[::1]:"))
        sock = socket.create_connection(("::1", mock.port), timeout=TIMEOUT)
        sock.settimeout(TIMEOUT)
        try:
            payload = json.dumps(valid_body()).encode("utf-8")
            sock.sendall(request_bytes("POST", "/v1/messages", api_headers("FAKE-test-key", payload), payload))
            self.assertEqual(read_reply(sock).status, 200)
        finally:
            sock.close()

    def test_cli_refuses_other_hosts(self):
        for host in ("0.0.0.0", "localhost", "192.168.1.5"):
            with self.subTest(host=host):
                done = subprocess.run([sys.executable, MOCK_PATH, "--host", host], stdin=subprocess.DEVNULL,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=TIMEOUT,
                                      env=child_env())
                self.assertEqual(done.returncode, 2)
                self.assertNotIn(b"LISTEN", done.stdout)
                self.assertIn(b"loopback", done.stderr)


# --------------------------------------------------------------------------------------------
# Documented defaults and scenario validation
# --------------------------------------------------------------------------------------------

class DefaultsTests(unittest.TestCase):
    def test_documented_defaults(self):
        args = ml.build_parser().parse_args([])
        self.assertEqual((args.host, args.port, args.default), ("127.0.0.1", 0, "basic"))
        self.assertEqual(args.min_cacheable_tokens, 512)
        self.assertEqual(args.cache_ttl, 300.0)
        emulator = ml.CacheEmulator()
        self.assertEqual((emulator.min_tokens, emulator.ttl), (512, 300.0))
        mock = ml.MockLlm()
        self.assertEqual((mock.cache.min_tokens, mock.cache.ttl), (512, 300.0))

    def test_every_must_be_a_whole_number_of_at_least_one(self):
        for bad in (0.5, 0, -1, True, "2"):
            with self.subTest(every=bad):
                with self.assertRaises(ValueError):
                    ml.validate_scenario({"steps": [{"raw": "abc", "every": bad}]})
        ml.validate_scenario({"steps": [{"raw": "abc", "every": 1, "delay_ms": 0.5, "piece_delay_ms": 1.5}]})
        with self.assertRaises(ValueError):
            ml.validate_scenario({"steps": [{"raw": "abc", "delay_ms": -1}]})

    def test_cli_rejects_an_out_of_range_port_with_a_usage_error(self):
        for port in ("70000", "-1"):
            with self.subTest(port=port):
                done = subprocess.run([sys.executable, MOCK_PATH, "--port", port], stdin=subprocess.DEVNULL,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=TIMEOUT,
                                      env=child_env())
                self.assertEqual(done.returncode, 2)
                self.assertNotIn(b"Traceback", done.stderr)
                self.assertNotIn(b"LISTEN", done.stdout)


# --------------------------------------------------------------------------------------------
# Command line and fixtures
# --------------------------------------------------------------------------------------------

class Addr:
    """Just enough of MockLlm (host and port) for the raw client helpers to talk to a CLI child."""

    host = "127.0.0.1"

    def __init__(self, port: int):
        self.port = port


def child_env() -> Dict[str, str]:
    env = dict(os.environ)
    env.update({"PYTHONUTF8": "1", "PYTHONUNBUFFERED": "1"})
    return env


def read_line_timeout(stream: Any, timeout: float) -> bytes:
    box: List[bytes] = []
    thread = threading.Thread(target=lambda: box.append(stream.readline()), daemon=True)
    thread.start()
    thread.join(timeout)
    if thread.is_alive() or not box:
        raise AssertionError("no line from the child within %.0f s" % timeout)
    return box[0]


def start_cli(args: List[str], stdin: Any = subprocess.PIPE) -> Tuple["subprocess.Popen[bytes]", int]:
    proc = subprocess.Popen([sys.executable, MOCK_PATH] + args, stdin=stdin, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=child_env())
    try:
        line = read_line_timeout(proc.stdout, TIMEOUT).decode("utf-8").strip()
    except AssertionError:
        proc.kill()
        raise
    match = re.match(r"^LISTEN 127\.0\.0\.1:(\d+)$", line)
    if not match:
        proc.kill()
        raise AssertionError("unexpected first line %r" % line)
    return proc, int(match.group(1))


def quit_cli(mock_like: Any) -> None:
    sock = connect(mock_like)
    try:
        sock.sendall(request_bytes("POST", "/__mock/quit", [("Host", "127.0.0.1"), ("Content-Length", "0")], b""))
        assert read_reply(sock).status == 200
    finally:
        sock.close()


def finish_cli(proc: "subprocess.Popen[bytes]") -> Tuple[bytes, bytes]:
    proc.wait(timeout=TIMEOUT)
    out = proc.stdout.read()  # type: ignore[union-attr]
    err = proc.stderr.read()  # type: ignore[union-attr]
    for pipe in (proc.stdin, proc.stdout, proc.stderr):
        if pipe is not None:
            pipe.close()
    return out, err


class CliTests(unittest.TestCase):
    def test_listen_serve_quit_endpoint_and_done(self):
        proc, port = start_cli(["--scenario", "basic"])  # stdin stays open: only /__mock/quit ends it
        try:
            mock_like = Addr(port)
            reply = post(mock_like, key="FAKE-test-key")
            self.assertEqual(text_of(parse_sse(reply.body)), ANSWER)
            quit_cli(mock_like)
            out, err = finish_cli(proc)
        finally:
            if proc.poll() is None:
                proc.kill()
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(out.decode("utf-8").splitlines(), ["DONE"])  # LISTEN was read by start_cli
        self.assertEqual(err, b"")

    def test_closing_stdin_ends_the_server(self):
        proc, _ = start_cli([])
        try:
            proc.stdin.close()  # type: ignore[union-attr]
            out, _ = finish_cli(proc)
        finally:
            if proc.poll() is None:
                proc.kill()
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(out.decode("utf-8").splitlines()[-1], "DONE")

    def test_ignore_stdin_keeps_it_running_after_stdin_closed(self):
        proc, port = start_cli(["--ignore-stdin"], stdin=subprocess.DEVNULL)
        try:
            mock_like = Addr(port)
            self.assertEqual(post(mock_like).status, 200)
            self.assertIsNone(proc.poll())
            quit_cli(mock_like)
            out, _ = finish_cli(proc)
        finally:
            if proc.poll() is None:
                proc.kill()
        self.assertEqual(out.decode("utf-8").splitlines()[-1], "DONE")

    @unittest.skipUnless(os.name == "posix", "SIGTERM delivery is a POSIX notion; Windows uses the quit endpoint")
    def test_sigterm_ends_the_server_with_done(self):
        proc, _ = start_cli([])
        try:
            proc.send_signal(signal.SIGTERM)
            out, _ = finish_cli(proc)
        finally:
            if proc.poll() is None:
                proc.kill()
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(out.decode("utf-8").splitlines()[-1], "DONE")

    def test_scenario_queue_default_and_record_dir_flags(self):
        with tempfile.TemporaryDirectory() as directory:
            proc, port = start_cli(["--scenario", "http_500", "--scenario", "http_429_retry_after",
                                    "--record-dir", directory, "--min-cacheable-tokens", "4", "--cache-ttl", "9"])
            try:
                mock_like = Addr(port)
                self.assertEqual([post(mock_like).status for _ in range(3)], [500, 429, 429])
                quit_cli(mock_like)
                finish_cli(proc)
            finally:
                if proc.poll() is None:
                    proc.kill()
            self.assertEqual(sorted(os.listdir(directory)), ["request-0000.json", "request-0001.json",
                                                             "request-0002.json"])

    def test_scenario_file_flag_and_decoy_url(self):
        path = os.path.join(HERE, "scenarios", "example.json")
        proc, port = start_cli(["--scenario", path, "--decoy-url", "http://127.0.0.1:1/x"])
        try:
            mock_like = Addr(port)
            self.assertEqual(text_of(parse_sse(post(mock_like).body)), "Test ruler one says hello.")
            self.assertEqual(post(mock_like, valid_body(model="mock:redirect_307")).headers["location"],
                             "http://127.0.0.1:1/x")
            quit_cli(mock_like)
            finish_cli(proc)
        finally:
            if proc.poll() is None:
                proc.kill()

    def test_bad_scenario_name_exits_with_an_error(self):
        done = subprocess.run([sys.executable, MOCK_PATH, "--scenario", "no_such_one"], stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=TIMEOUT, env=child_env())
        self.assertEqual(done.returncode, 2)
        self.assertNotIn(b"LISTEN", done.stdout)

    def test_list_prints_the_builtin_names(self):
        done = subprocess.run([sys.executable, MOCK_PATH, "--list"], stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, timeout=TIMEOUT, env=child_env())
        self.assertEqual(done.stdout.decode("utf-8").split(), ml.builtin_names())


class FixtureTests(unittest.TestCase):
    def fixture(self, name: str) -> bytes:
        with open(os.path.join(FIXTURES, name + ".sse"), "rb") as handle:
            return handle.read()

    def test_the_committed_fixtures_are_exactly_what_the_mock_emits(self):
        for name in ml.FIXTURE_NAMES:
            with self.subTest(name):
                self.assertEqual(self.fixture(name), ml.wire_body(name))

    def test_no_stray_fixture_files(self):
        files = sorted(f for f in os.listdir(FIXTURES) if f.endswith(".sse"))
        self.assertEqual(files, sorted(n + ".sse" for n in ml.FIXTURE_NAMES))

    def test_cli_emit_reproduces_every_fixture_byte_for_byte(self):
        for name in ml.FIXTURE_NAMES:
            with self.subTest(name):
                done = subprocess.run([sys.executable, MOCK_PATH, "--emit", name], stdin=subprocess.DEVNULL,
                                      stdout=subprocess.PIPE, timeout=TIMEOUT, env=child_env())
                self.assertEqual(done.returncode, 0)
                self.assertEqual(done.stdout, self.fixture(name))

    def test_emit_out_and_write_fixtures(self):
        with tempfile.TemporaryDirectory() as directory:
            target = os.path.join(directory, "one.sse")
            subprocess.run([sys.executable, MOCK_PATH, "--emit", "crlf", "--out", target], check=True,
                           stdin=subprocess.DEVNULL, timeout=TIMEOUT, env=child_env())
            with open(target, "rb") as handle:
                self.assertEqual(handle.read(), self.fixture("crlf"))
            subprocess.run([sys.executable, MOCK_PATH, "--write-fixtures", directory], check=True,
                           stdin=subprocess.DEVNULL, timeout=TIMEOUT, env=child_env())
            for name in ml.FIXTURE_NAMES:
                with open(os.path.join(directory, name + ".sse"), "rb") as handle:
                    self.assertEqual(handle.read(), self.fixture(name))

    def test_emit_is_deterministic(self):
        for name in ml.FIXTURE_NAMES:
            self.assertEqual(ml.wire_body(name), ml.wire_body(name))

    def test_what_each_fixture_contains(self):
        events = {name: parse_sse(self.fixture(name)) for name in ml.FIXTURE_NAMES}
        for name in ("basic", "ping_interleaved", "unknown_event", "crlf", "thinking_blocks", "usage_both",
                     "usage_start_only", "usage_delta_only", "max_tokens"):
            self.assertEqual(text_of(events[name]), ANSWER, name)
            self.assertEqual(events[name][-1]["event"], "message_stop", name)
        self.assertEqual(text_of(events["split_every_byte"]), UTF8_ANSWER)
        self.assertIn(b"\r\n", self.fixture("crlf"))
        self.assertNotIn(b"\r", self.fixture("basic"))
        self.assertIn("ping", names_of(events["ping_interleaved"]))
        self.assertIn("mock_future_event", names_of(events["unknown_event"]))
        self.assertIn("signature_delta", self.fixture("thinking_blocks").decode("utf-8"))
        self.assertEqual(events["midstream_error"][-1]["event"], "error")
        self.assertNotIn("message_stop", names_of(events["truncated"]))
        self.assertNotIn("message_stop", names_of(events["midstream_error"]))
        self.assertEqual(events["refusal"][-2]["json"]["delta"]["stop_reason"], "refusal")
        self.assertEqual(events["max_tokens"][-2]["json"]["delta"]["stop_reason"], "max_tokens")
        self.assertEqual([e["json"] is None for e in events["bad_json"]].count(True), 1)
        self.assertIsNone(final_usage(events["usage_start_only"]))
        self.assertIsNone(start_usage(events["usage_delta_only"]))
        self.assertEqual(final_usage(events["usage_both"])["cache_read_input_tokens"], 120)

    def test_fixtures_are_synthetic(self):
        for name in ml.FIXTURE_NAMES:
            text = self.fixture(name).decode("utf-8")
            for event in parse_sse(text.encode("utf-8")):
                j = event["json"]
                if isinstance(j, dict) and j.get("type") == "message_start":
                    self.assertEqual(j["message"]["id"], "msg_mock0001")
                    self.assertEqual(j["message"]["model"], "mock-model-1")

    def test_no_real_model_ids_or_key_shapes_in_this_tool(self):
        # The needle is assembled so that this file does not contain it itself.
        needle = ("clau" + "de-").encode("ascii")
        key_shape = re.compile(rb"sk-[A-Za-z0-9_-]{20,}")
        folders = [HERE, FIXTURES]
        for folder in folders:
            for root, dirs, files in os.walk(folder):
                dirs[:] = [d for d in dirs if d != "__pycache__"]  # compiled files fold the needle together
                for name in files:
                    with open(os.path.join(root, name), "rb") as handle:
                        data = handle.read()
                    self.assertNotIn(needle, data.lower(), name)
                    self.assertIsNone(key_shape.search(data), name)
                    self.assertNotIn(b"/Us" + b"ers/", data, name)


if __name__ == "__main__":
    unittest.main()
