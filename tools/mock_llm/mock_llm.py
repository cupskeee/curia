#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A mock Anthropic Messages streaming server, for tests that must run without the real API.

It plays *scenarios* (SSE streams, HTTP errors, stalls, resets) over a raw socket server that the
mock owns, so a test controls exactly where every TCP send lands. Every request is recorded, with
protocol violations listed but never rejected, and the API key never leaves the mock in clear text:
header values and any other occurrence of the same string are replaced by ``key_digest(value)``
in the records, the ``/__mock/requests`` endpoint, ``--record-dir`` files and log lines. (The
``echo_key`` scenarios reflect the real key into the *response* on purpose: that is how a test
proves the client under test redacts it.)

Everything is synthetic: model ``mock-model-1``, message id ``msg_mock0001``, artificial text.
Real API captures must never be committed; they belong under the gitignored ``reference/``.

Use it as a command (see ``--help`` and README.md) or as a library (``MockLlm``). Standard library
only; written for Python 3.9 to 3.14 on Windows, macOS and Linux: raw sockets with timeouts,
threads, no ``fork``, no ``fcntl``, no ``SO_REUSEADDR`` (its Windows meaning allows port hijacking),
loopback addresses only.
"""
from __future__ import annotations

import argparse
import base64
import collections
import copy
import hashlib
import json
import os
import re
import select
import signal
import socket
import struct
import sys
import threading
import time
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence, Tuple, Union

MODEL = "mock-model-1"
MESSAGE_ID = "msg_mock0001"
LOOPBACK_HOSTS = ("127.0.0.1", "::1")
DEFAULT_DECOY_URL = "http://127.0.0.1:9/decoy"
MAX_HEAD_BYTES = 64 * 1024
MAX_BODY_BYTES = 32 * 1024 * 1024  # the real API's request limit
FORBIDDEN_KEYS = ("budget_tokens", "temperature", "top_p", "top_k", "tool_choice")
# The optional settings Curia may send, only at the top level and only in this shape:
# "thinking": {"type": <one of THINKING_TYPES>} and "output_config": {"effort": <one of EFFORTS>}.
THINKING_TYPES = ("adaptive", "between_tools", "disabled")
EFFORTS = ("low", "medium", "high", "xhigh", "max")
# The markers of the realm block the app renders (app/src/advisor/snapshot_renderer.h).
REALM_BEGIN = "=== WHAT THE ADVISOR CAN SEE ==="
REALM_END = "=== END OF WHAT THE ADVISOR CAN SEE ==="
REALM_NONE_YET = "Nothing has been read from the game yet"
SENSITIVE_HEADERS = ("authorization", "x-api-key", "proxy-authorization")
STEP_KINDS = ("sse", "raw", "b64", "text", "gen", "hold_ms", "close", "stall", "loop")
STEP_OPTIONS = ("chunks", "every", "delay_ms", "piece_delay_ms", "split_mid_utf8", "eol")
REASONS = {
    100: "Continue", 200: "OK", 307: "Temporary Redirect", 400: "Bad Request",
    401: "Unauthorized", 403: "Forbidden", 404: "Not Found", 413: "Payload Too Large",
    429: "Too Many Requests", 431: "Request Header Fields Too Large",
    500: "Internal Server Error", 529: "Overloaded",
}


def key_digest(value: Union[str, bytes]) -> str:
    """What stands in for a secret in every record, endpoint output, file and log line."""
    raw = value if isinstance(value, bytes) else value.encode("utf-8")
    return "sha256:%s(len=%d)" % (hashlib.sha256(raw).hexdigest()[:12], len(raw))


def _json(obj: Any, ascii_only: bool = False) -> str:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=ascii_only)


def error_body(kind: str, message: str) -> Dict[str, Any]:
    """The Anthropic error shape."""
    return {"type": "error", "error": {"type": kind, "message": message}}


# --------------------------------------------------------------------------------------------
# Scenario building blocks
# --------------------------------------------------------------------------------------------

DEFAULT_PARTS = ["Test answer", " from the", " mock server."]
DEFAULT_THINKING_PARTS = ["Test thought one.", " Test thought two."]
DEFAULT_USAGE = {
    "input_tokens": 25,
    "cache_creation_input_tokens": 0,
    "cache_read_input_tokens": 0,
    "output_tokens": 12,
}


def sse(event: Optional[str], data: Any, **options: Any) -> Dict[str, Any]:
    step: Dict[str, Any] = {"sse": {"event": event, "data": data}}
    step.update(options)
    return step


def answer_steps(p: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The events of one streamed answer, as ``sse`` steps (the ``gen: answer`` generator).

    ``usage_in`` says where the usage numbers appear: ``start`` (message_start holds all of them,
    message_delta has no usage), ``delta`` (message_start has no usage, message_delta holds all) or
    ``both`` (message_start holds the input and cache numbers with output_tokens 1, message_delta
    holds the cumulative totals; the shape of the real API).
    """
    parts = list(p.get("text_parts", DEFAULT_PARTS))
    thinking_parts = list(p.get("thinking_parts", DEFAULT_THINKING_PARTS))
    usage = dict(p.get("usage") or DEFAULT_USAGE)
    usage.setdefault("output_tokens", 12)
    order = ("input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens", "output_tokens")
    full = {k: usage[k] for k in order if k in usage}
    full.update({k: v for k, v in usage.items() if k not in full})
    usage_in = p.get("usage_in", "both")
    if usage_in == "start":
        start_usage: Optional[Dict[str, Any]] = full
        delta_usage: Optional[Dict[str, Any]] = None
    elif usage_in == "delta":
        start_usage, delta_usage = None, full
    elif usage_in == "both":
        start_usage, delta_usage = dict(full, output_tokens=1), full
    else:
        raise ValueError("usage_in must be start, delta or both")
    message: Dict[str, Any] = {
        "id": p.get("id", MESSAGE_ID), "type": "message", "role": "assistant",
        "model": p.get("model", MODEL), "content": [], "stop_reason": None, "stop_sequence": None,
    }
    if start_usage is not None:
        message["usage"] = start_usage
    events: List[Tuple[str, Dict[str, Any]]] = [("message_start", {"type": "message_start", "message": message})]
    index = 0
    if p.get("thinking"):
        events.append(("content_block_start", {"type": "content_block_start", "index": index,
                                               "content_block": {"type": "thinking", "thinking": ""}}))
        for part in thinking_parts:
            events.append(("content_block_delta", {"type": "content_block_delta", "index": index,
                                                   "delta": {"type": "thinking_delta", "thinking": part}}))
        events.append(("content_block_delta", {"type": "content_block_delta", "index": index,
                                               "delta": {"type": "signature_delta",
                                                         "signature": "mock-signature-0001"}}))
        events.append(("content_block_stop", {"type": "content_block_stop", "index": index}))
        index += 1
    if parts:
        events.append(("content_block_start", {"type": "content_block_start", "index": index,
                                               "content_block": {"type": "text", "text": ""}}))
        for part in parts:
            events.append(("content_block_delta", {"type": "content_block_delta", "index": index,
                                                   "delta": {"type": "text_delta", "text": part}}))
        events.append(("content_block_stop", {"type": "content_block_stop", "index": index}))
    delta: Dict[str, Any] = {"type": "message_delta",
                             "delta": {"stop_reason": p.get("stop_reason", "end_turn"), "stop_sequence": None}}
    if delta_usage is not None:
        delta["usage"] = delta_usage
    events.append(("message_delta", delta))
    events.append(("message_stop", {"type": "message_stop"}))
    ping_every = int(p.get("ping_every", 0))
    if ping_every > 0:
        spaced: List[Tuple[str, Dict[str, Any]]] = []
        for number, event in enumerate(events):
            spaced.append(event)
            if number + 1 < len(events) and (number + 1) % ping_every == 0:
                spaced.append(("ping", {"type": "ping"}))
        events = spaced
    return [sse(name, data) for name, data in events]


def _http_error(status: int, kind: str, message: str, headers: Optional[Dict[str, str]] = None) -> Dict[str, Any]:
    scenario: Dict[str, Any] = {"status": status, "steps": [{"text": error_body(kind, message)}]}
    if headers:
        scenario["headers"] = headers
    return scenario


class RenderContext:
    """What a scenario builder may look at: the request, the received key, the decoy URL."""

    def __init__(self, request: Any = None, key: str = "", decoy_url: str = DEFAULT_DECOY_URL,
                 cache_fn: Optional[Callable[[], Dict[str, int]]] = None, arg: Optional[str] = None):
        self.arg = arg
        self.request = request
        self.key = key
        self.decoy_url = decoy_url
        self._cache_fn = cache_fn

    def cache_usage(self) -> Dict[str, int]:
        if self._cache_fn is not None:
            return self._cache_fn()
        return {"input_tokens": 30, "cache_creation_input_tokens": 600, "cache_read_input_tokens": 0}

    def system_text(self) -> str:
        system = self.request.get("system") if isinstance(self.request, dict) else None
        if isinstance(system, str):
            return system
        blocks = system if isinstance(system, list) else []
        return "\n".join(b["text"] for b in blocks if isinstance(b, dict) and isinstance(b.get("text"), str))


def _gen(**params: Any) -> Dict[str, Any]:
    return dict({"gen": "answer"}, **params)


_UTF8_PARTS = ["Test café", " costs €5", " and \U0001F600", " done."]  # 2-, 3- and 4-byte characters
_FULL_USAGE = {"input_tokens": 21, "cache_creation_input_tokens": 340, "cache_read_input_tokens": 120,
               "output_tokens": 15}


def realm_block(text: str) -> Optional[str]:
    """The text between the app's realm markers (REALM_BEGIN, REALM_END), or None."""
    start = text.find(REALM_BEGIN)
    if start < 0:
        return None
    end = text.find(REALM_END, start + len(REALM_BEGIN))
    return text[start + len(REALM_BEGIN):end] if end >= 0 else None


def _b_cite(ctx: RenderContext) -> Dict[str, Any]:
    """Quotes the gold figure and the heir's name found in the system blocks. It understands the
    app's rendered realm block (between "=== WHAT THE ADVISOR CAN SEE ===" and "=== END OF WHAT
    THE ADVISOR CAN SEE ===": "Treasury: 312 gold; ..." and 'Heir: "Name"; ...', looked for inside
    the markers only), answers "Nothing has been read yet." to the block that says nothing has
    been read, and, without the markers, reads plain "gold: N" / "heir: Name" lines. The usage
    numbers come from the cache emulator, so a second request with the same system prefix reports
    cache_read_input_tokens."""
    whole = ctx.system_text()
    block = realm_block(whole)
    usage = dict(ctx.cache_usage(), output_tokens=12)
    if block is not None and REALM_NONE_YET in block:
        return {"steps": [_gen(text_parts=["Nothing has been read yet."], usage=usage)]}
    text = block if block is not None else whole
    gold = (re.search(r"(?m)^Treasury:[ \t]*(-?\d+) gold", text)
            or re.search(r"(?im)^[ \t]*gold:[ \t]*(-?\d+)", text))
    heir = (re.search(r'(?m)^Heir:[ \t]*"([^"]*)"', text)
            or re.search(r"(?im)^[ \t]*heir:[ \t]*(.+?)[ \t]*$", text))
    parts = ["Gold is ", gold.group(1) if gold else "unknown", ". Heir is ",
             heir.group(1) if heir else "unknown", "."]
    return {"steps": [_gen(text_parts=parts, usage=usage)]}


def _b_echo_key_split(ctx: RenderContext) -> Dict[str, Any]:
    """Streams the received key back in two text deltas, cut after ``arg`` characters of the key
    (default 12), and puts the cut on a TCP boundary too: the first delta's bytes end exactly after
    the first part of the key. A client must redact the joined text, not only each token."""
    key = ctx.key or "no-key-received"
    try:
        offset = int(ctx.arg) if ctx.arg else 12
    except ValueError:
        offset = 12
    offset = max(1, min(offset, len(key) - 1)) if len(key) > 1 else 1
    first_text, second_text = "The key is " + key[:offset], key[offset:] + " and that is all."

    def delta(text: str) -> Dict[str, Any]:
        return {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": text}}

    skeleton = answer_steps({"text_parts": ["x"]})  # start, block start, one delta, stop, delta, stop
    steps = skeleton[:2]
    first = sse("content_block_delta", delta(first_text))
    wire = step_bytes(first) or b""
    # Cut right after the key's first part inside the JSON string (before its closing quote).
    cut = wire.index(first_text.encode("utf-8")) + len(first_text.encode("utf-8"))
    first["chunks"] = [cut]
    first["piece_delay_ms"] = 60
    second = sse("content_block_delta", delta(second_text))
    second["delay_ms"] = 60
    return {"steps": steps + [first, second] + skeleton[-3:]}


def _half_key_head(ctx: RenderContext) -> List[Dict[str, Any]]:
    """message_start, a text block start and one delta holding only the first ``arg`` characters of
    the received key (default 20): the stream is then cut, so the key never arrives whole."""
    key = ctx.key or "no-key-received"
    try:
        offset = int(ctx.arg) if ctx.arg else 20
    except ValueError:
        offset = 20
    offset = max(1, min(offset, len(key) - 1)) if len(key) > 1 else 1
    skeleton = answer_steps({"text_parts": ["x"]})
    delta = {"type": "content_block_delta", "index": 0,
             "delta": {"type": "text_delta", "text": "The key is " + key[:offset]}}
    return skeleton[:2] + [sse("content_block_delta", delta)]


def _b_echo_key_half(ctx: RenderContext) -> Dict[str, Any]:
    """The first part of the key in one delta, then silence until the client closes (a cancel
    leaves the key cut in two)."""
    return {"steps": _half_key_head(ctx) + [{"hold_ms": 120000, "until_client_close": True}]}


def _b_echo_key_half_close(ctx: RenderContext) -> Dict[str, Any]:
    """The first part of the key in one delta, then the connection is cut (a network error with
    the key cut in two in the partial text)."""
    return {"steps": _half_key_head(ctx) + [{"close": "truncate"}]}


def _partial_stream(extra: Sequence[Dict[str, Any]], tokens: int = 3) -> List[Dict[str, Any]]:
    """message_start, a text block start and ``tokens`` deltas, then ``extra`` steps."""
    steps = answer_steps({"text_parts": ["Test token %d." % n for n in range(1, tokens + 1)]})
    return steps[:2 + tokens] + list(extra)


def _b_cache_demo(ctx: RenderContext) -> Dict[str, Any]:
    usage = dict(ctx.cache_usage(), output_tokens=12)
    return {"steps": [_gen(usage=usage)]}


def _b_echo_key(ctx: RenderContext) -> Dict[str, Any]:
    steps = answer_steps({"text_parts": ["The key is {key}"]})[:3]
    steps.append(sse("error", error_body("api_error", "mock rejected key {key}")))
    return {"steps": steps}


def _b_slow_forever(ctx: RenderContext) -> Dict[str, Any]:
    head = answer_steps({})[:2]
    loop = {"loop": sse("content_block_delta", {"type": "content_block_delta", "index": 0,
                                                "delta": {"type": "text_delta", "text": "tok "}},
                        delay_ms=100), "max_ms": 120000}
    return {"steps": head + [loop]}


def _b_unknown_event(ctx: RenderContext) -> Dict[str, Any]:
    steps = answer_steps({})
    future = sse("mock_future_event", {"type": "mock_future_event", "value": 1})
    odd_delta = sse("content_block_delta", {"type": "content_block_delta", "index": 0,
                                            "delta": {"type": "mock_future_delta", "value": 2}})
    return {"steps": steps[:1] + [future] + steps[1:3] + [odd_delta] + steps[3:]}


def _b_bad_json(ctx: RenderContext) -> Dict[str, Any]:
    steps = answer_steps({})
    bad = {"sse": {"event": "content_block_delta", "data": "{not json"}}
    return {"steps": steps[:2] + [bad] + steps[2:]}


def _b_midstream_error(ctx: RenderContext) -> Dict[str, Any]:
    error = sse("error", error_body("overloaded_error", "Mock overloaded"))
    return {"steps": _partial_stream([error, {"close": "clean"}])}


# Builtins: name -> scenario dict, or a builder taking a RenderContext.
BUILTINS: Dict[str, Union[Dict[str, Any], Callable[[RenderContext], Dict[str, Any]]]] = {
    "basic": {"steps": [_gen()]},
    "ping_interleaved": {"steps": [_gen(ping_every=1)]},
    "unknown_event": _b_unknown_event,
    "split_every_byte": {"steps": [_gen(text_parts=_UTF8_PARTS, every=1)]},
    "crlf": {"steps": [_gen(eol="\r\n")]},
    "midstream_error": _b_midstream_error,
    "truncated": {"steps": _partial_stream([{"close": "truncate"}], tokens=2)},
    "thinking_blocks": {"steps": [_gen(thinking=True)]},
    "refusal": {"steps": [_gen(text_parts=["Test refusal."], stop_reason="refusal")]},
    "max_tokens": {"steps": [_gen(stop_reason="max_tokens", usage={"input_tokens": 25, "output_tokens": 4096})]},
    "bad_json": _b_bad_json,
    "usage_start_only": {"steps": [_gen(usage=_FULL_USAGE, usage_in="start")]},
    "usage_delta_only": {"steps": [_gen(usage=_FULL_USAGE, usage_in="delta")]},
    "usage_both": {"steps": [_gen(usage=_FULL_USAGE, usage_in="both")]},
    "unicode_escaped": {"steps": [_gen(text_parts=_UTF8_PARTS, ascii=True)]},
    "basic_close_framing": {"framing": "close", "steps": [_gen()]},
    "http_400": _http_error(400, "invalid_request_error", "Mock invalid request"),
    "http_401": _http_error(401, "authentication_error", "Mock invalid credentials"),
    "http_403": _http_error(403, "permission_error", "Mock permission denied"),
    "http_404": _http_error(404, "not_found_error", "Mock model not found"),
    "http_413": _http_error(413, "request_too_large", "Mock request too large"),
    "http_429_retry_after": _http_error(429, "rate_limit_error", "Mock rate limited", {"retry-after": "7"}),
    "http_500": _http_error(500, "api_error", "Mock internal error"),
    "http_529": _http_error(529, "overloaded_error", "Mock overloaded"),
    "stall": {"steps": _partial_stream([{"stall": True}], tokens=1)},
    "slow_forever": _b_slow_forever,
    "hold_after_tokens": {"steps": _partial_stream([{"hold_ms": 120000, "until_client_close": True}])},
    "reset_midstream": {"steps": _partial_stream([{"close": "reset"}])},
    "echo_key": _b_echo_key,
    "echo_key_http": {"status": 401, "steps": [{"text": error_body("authentication_error", "Mock bad key {key}")}]},
    "redirect_307": {"status": 307, "headers": {"location": "{decoy_url}"},
                     "steps": [{"text": error_body("api_error", "Mock redirect")}]},
    "cite": _b_cite,
    "echo_key_split": _b_echo_key_split,
    "echo_key_half": _b_echo_key_half,
    "echo_key_half_close": _b_echo_key_half_close,
    "cache_demo": _b_cache_demo,
}

# The deterministic scenarios whose SSE bytes are committed under tests/fixtures/anthropic/.
FIXTURE_NAMES = (
    "basic", "ping_interleaved", "unknown_event", "split_every_byte", "crlf", "midstream_error",
    "truncated", "thinking_blocks", "refusal", "max_tokens", "bad_json", "usage_start_only",
    "usage_delta_only", "usage_both",
)


def builtin_names() -> List[str]:
    return sorted(BUILTINS)


# --------------------------------------------------------------------------------------------
# Scenario handling
# --------------------------------------------------------------------------------------------

def validate_scenario(scenario: Any) -> None:
    """Raises ValueError for a malformed scenario (checked when it is added, not when played)."""
    if not isinstance(scenario, dict):
        raise ValueError("a scenario is a JSON object")
    status = scenario.get("status", 200)
    if not isinstance(status, int) or isinstance(status, bool) or not 100 <= status <= 599:
        raise ValueError("status must be an integer from 100 to 599")
    if scenario.get("framing", "chunked") not in ("chunked", "close"):
        raise ValueError("framing must be chunked or close")
    if not isinstance(scenario.get("headers", {}), dict):
        raise ValueError("headers must be an object")
    steps = scenario.get("steps", [])
    if not isinstance(steps, list):
        raise ValueError("steps must be a list")
    for step in steps:
        _validate_step(step)


def _validate_step(step: Any) -> None:
    if not isinstance(step, dict):
        raise ValueError("a step is a JSON object")
    kinds = [k for k in step if k in STEP_KINDS]
    if len(kinds) != 1:
        raise ValueError("a step has exactly one of %s: %r" % (", ".join(STEP_KINDS), sorted(step)))
    kind = kinds[0]
    allowed = set(STEP_KINDS) | set(STEP_OPTIONS)
    if kind == "hold_ms":
        allowed.add("until_client_close")
    if kind == "loop":
        allowed |= {"max_ms", "count"}
    if kind == "gen":
        allowed |= {"text_parts", "thinking_parts", "usage", "usage_in", "stop_reason", "thinking",
                    "ping_every", "ascii", "id", "model"}
    if kind == "sse":
        allowed.add("ascii")
    unknown = sorted(set(step) - allowed)
    if unknown:
        raise ValueError("unknown step keys: %s" % ", ".join(unknown))
    if kind == "sse":
        body = step["sse"]
        if not isinstance(body, dict) or "data" not in body:
            raise ValueError("sse needs {'event': ..., 'data': ...}")
    elif kind == "gen" and step["gen"] != "answer":
        raise ValueError("the only generator is 'answer'")
    elif kind == "b64":
        base64.b64decode(step["b64"], validate=True)
    elif kind == "close" and step["close"] not in ("clean", "reset", "truncate"):
        raise ValueError("close must be clean, reset or truncate")
    elif kind == "loop":
        _validate_step(step["loop"])
    for name in ("delay_ms", "piece_delay_ms"):
        if name in step and (isinstance(step[name], bool) or not isinstance(step[name], (int, float))
                             or step[name] < 0):
            raise ValueError("%s must be a non-negative number" % name)
    if "every" in step and (isinstance(step["every"], bool) or not isinstance(step["every"], int)
                            or step["every"] < 1):
        raise ValueError("every must be an integer of at least 1")
    if "chunks" in step and not (isinstance(step["chunks"], list)
                                 and all(isinstance(n, int) and n > 0 for n in step["chunks"])):
        raise ValueError("chunks must be a list of positive integers")


def load_scenario_file(path: str) -> List[Dict[str, Any]]:
    """A JSON file holding one scenario object or a list of them."""
    with open(path, "rb") as handle:
        data = json.loads(handle.read().decode("utf-8"))
    scenarios = data if isinstance(data, list) else [data]
    for scenario in scenarios:
        validate_scenario(scenario)
        if not scenario.get("name"):
            scenario["name"] = os.path.splitext(os.path.basename(path))[0]
    return scenarios


def _substitute(obj: Any, ctx: RenderContext) -> Any:
    if isinstance(obj, str):
        return obj.replace("{decoy_url}", ctx.decoy_url).replace("{key}", ctx.key)
    if isinstance(obj, list):
        return [_substitute(x, ctx) for x in obj]
    if isinstance(obj, dict):
        return {k: _substitute(v, ctx) for k, v in obj.items()}
    return obj


def _expand_steps(steps: Iterable[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Replaces generator steps by the sse steps they stand for; options pass to every event."""
    out: List[Dict[str, Any]] = []
    for step in steps:
        if "gen" in step:
            carried = {k: step[k] for k in STEP_OPTIONS + ("ascii",) if k in step}
            for event in answer_steps(step):
                event.update(carried)
                out.append(event)
        elif "loop" in step:
            inner = _expand_steps([step["loop"]])
            copy_step = dict(step)
            copy_step["loop"] = inner[0]
            out.append(copy_step)
        else:
            out.append(step)
    return out


def resolve_scenario(source: Any, ctx: RenderContext, name: str = "custom") -> Dict[str, Any]:
    """Runs a builder (if any), substitutes ``{key}`` and ``{decoy_url}``, expands generators."""
    scenario = source(ctx) if callable(source) else copy.deepcopy(source)
    validate_scenario(scenario)
    scenario = _substitute(scenario, ctx)
    return {
        "name": scenario.get("name", name),
        "status": int(scenario.get("status", 200)),
        "headers": {str(k).lower(): str(v) for k, v in (scenario.get("headers") or {}).items()},
        "framing": scenario.get("framing", "chunked"),
        "steps": _expand_steps(scenario.get("steps") or []),
    }


def step_bytes(step: Dict[str, Any]) -> Optional[bytes]:
    """The wire bytes of a data step (sse, raw, b64, text), or None for a flow step."""
    if "sse" in step:
        body = step["sse"]
        eol = step.get("eol", "\n")
        data = body["data"]
        text = data if isinstance(data, str) else _json(data, bool(step.get("ascii")))
        lines = []
        if body.get("event") is not None:
            lines.append("event: %s" % body["event"])
        # A data string with line breaks becomes several data lines, as SSE requires.
        lines.extend("data: %s" % piece for piece in text.split("\n"))
        return (eol.join(lines) + eol + eol).encode("utf-8")
    if "raw" in step:
        return step["raw"].encode("utf-8")
    if "b64" in step:
        return base64.b64decode(step["b64"])
    if "text" in step:
        value = step["text"]
        return (value if isinstance(value, str) else _json(value)).encode("utf-8")
    return None


def split_pieces(data: bytes, step: Dict[str, Any]) -> List[bytes]:
    """Splits a step's bytes into the pieces that are sent separately (one TCP send each)."""
    chunks, every = step.get("chunks"), step.get("every")
    if chunks:
        pieces, pos = [], 0
        for size in chunks:
            if pos >= len(data):
                break
            pieces.append(data[pos:pos + size])
            pos += size
        if pos < len(data):
            pieces.append(data[pos:])
    elif every:
        size = int(every)
        pieces = [data[i:i + size] for i in range(0, len(data), size)]
    elif step.get("split_mid_utf8"):
        lead = next((i for i, b in enumerate(data) if b >= 0xC0), None)
        pieces = [data] if lead is None else [data[:lead + 1], data[lead + 1:]]
    else:
        pieces = [data]
    return [p for p in pieces if p]


def wire_body(name_or_scenario: Any, request: Any = None) -> bytes:
    """The exact SSE bytes of a scenario (no HTTP framing, flow steps skipped): what ``--emit`` prints."""
    ctx = RenderContext(request=request)
    source = BUILTINS[name_or_scenario] if isinstance(name_or_scenario, str) else name_or_scenario
    out = bytearray()
    for step in resolve_scenario(source, ctx)["steps"]:
        inner = step["loop"] if "loop" in step else step
        data = step_bytes(inner)
        if data is not None:
            out.extend(data)
    return bytes(out)


# --------------------------------------------------------------------------------------------
# Cache emulator
# --------------------------------------------------------------------------------------------

def _strip_cache_control(block: Any) -> Any:
    return {k: v for k, v in block.items() if k != "cache_control"} if isinstance(block, dict) else block


class CacheEmulator:
    """Prompt-cache bookkeeping in the shape of the real usage fields (an approximation).

    The prefix (tools, then system blocks, then message content blocks) up to each ``cache_control``
    marker is hashed as canonical sorted-keys JSON without the markers; tokens are estimated as
    ``len(utf8) // 4``. A prefix creates an entry only if it has at least ``min_tokens`` tokens.
    """

    def __init__(self, min_tokens: int = 512, ttl: float = 300.0, clock: Callable[[], float] = time.monotonic):
        self.min_tokens = min_tokens
        self.ttl = ttl
        self._clock = clock
        self._entries: Dict[str, float] = {}
        self._lock = threading.Lock()

    @staticmethod
    def _items(body: Dict[str, Any]) -> List[Tuple[str, Any]]:
        items: List[Tuple[str, Any]] = []
        for tool in body.get("tools") or []:
            items.append(("tool", tool))
        system = body.get("system")
        if isinstance(system, str):
            system = [{"type": "text", "text": system}]
        for block in system or []:
            items.append(("system", block))
        for message in body.get("messages") or []:
            if not isinstance(message, dict):
                continue
            content = message.get("content")
            if isinstance(content, str):
                content = [{"type": "text", "text": content}]
            for block in content or []:
                items.append(("message:%s" % message.get("role"), block))
        return items

    def usage(self, body: Any) -> Dict[str, int]:
        if not isinstance(body, dict):
            return {"input_tokens": 0, "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0}
        digest = hashlib.sha256(str(body.get("model")).encode("utf-8"))
        total_len = 0
        points: List[Tuple[str, int, float]] = []  # (prefix hash, prefix length, ttl)
        for where, block in self._items(body):
            canon = (where + ":" + json.dumps(_strip_cache_control(block), sort_keys=True,
                                              separators=(",", ":"), ensure_ascii=False)).encode("utf-8")
            digest.update(canon + b"\n")
            total_len += len(canon)
            marker = block.get("cache_control") if isinstance(block, dict) else None
            if isinstance(marker, dict) and len(points) < 4:
                ttl = 3600.0 if marker.get("ttl") == "1h" else self.ttl
                points.append((digest.copy().hexdigest(), total_len, ttl))
        total = total_len // 4
        with self._lock:
            now = self._clock()
            self._entries = {h: exp for h, exp in self._entries.items() if exp > now}
            if not points or points[-1][1] // 4 < self.min_tokens:
                return {"input_tokens": total, "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0}
            read = 0
            for prefix_hash, length, ttl in reversed(points):
                if prefix_hash in self._entries:
                    read = length // 4
                    self._entries[prefix_hash] = now + ttl
                    break
            last_tokens = points[-1][1] // 4
            for prefix_hash, length, ttl in points:
                if length // 4 >= self.min_tokens and prefix_hash not in self._entries:
                    self._entries[prefix_hash] = now + ttl
            return {
                "input_tokens": total - last_tokens,
                "cache_creation_input_tokens": last_tokens - read,
                "cache_read_input_tokens": read,
            }


# --------------------------------------------------------------------------------------------
# Request validation (records problems, never rejects)
# --------------------------------------------------------------------------------------------

def _walk_keys(obj: Any) -> Iterable[str]:
    if isinstance(obj, dict):
        for key, value in obj.items():
            yield key
            for inner in _walk_keys(value):
                yield inner
    elif isinstance(obj, list):
        for item in obj:
            for inner in _walk_keys(item):
                yield inner


def _walk_dicts(obj: Any) -> Iterable[Dict[str, Any]]:
    if isinstance(obj, dict):
        yield obj
        for value in obj.values():
            for inner in _walk_dicts(value):
                yield inner
    elif isinstance(obj, list):
        for item in obj:
            for inner in _walk_dicts(item):
                yield inner


def request_setting(body: Any, outer: str, inner: str) -> Optional[str]:
    """body[outer][inner] when it is a string (the request's thinking type or effort), else None."""
    value = body.get(outer) if isinstance(body, dict) else None
    word = value.get(inner) if isinstance(value, dict) else None
    return word if isinstance(word, str) else None


def validate_request(method: str, target: str, headers: Dict[str, List[str]], body: Any) -> List[str]:
    """The list of violations of the Messages request contract Curia's client must keep."""
    found: List[str] = []
    if method != "POST":
        found.append("wrong_method")
    if target.split("?", 1)[0] != "/v1/messages":
        found.append("wrong_path")
    if "anthropic-version" not in headers:
        found.append("missing_anthropic_version")
    content_type = headers.get("content-type")
    if not content_type:
        found.append("missing_content_type")
    elif not content_type[0].lower().startswith("application/json"):
        found.append("bad_content_type")
    if body is None:
        found.append("body_not_json")
        return found
    if not isinstance(body, dict):
        found.append("body_not_object")
        return found
    if not isinstance(body.get("model"), str) or not body["model"]:
        found.append("missing_model")
    max_tokens = body.get("max_tokens")
    if not isinstance(max_tokens, int) or isinstance(max_tokens, bool) or max_tokens < 1:
        found.append("missing_max_tokens")
    if body.get("stream") is not True:
        found.append("stream_not_true")
    if "system" in body:
        system = body["system"]
        if not (isinstance(system, list) and all(
                isinstance(b, dict) and b.get("type") == "text" and isinstance(b.get("text"), str)
                for b in system)):
            found.append("system_not_text_blocks")
        else:
            if any(not b["text"].strip() for b in system):
                found.append("empty_text_block")
    good = ({"type": "ephemeral"}, {"type": "ephemeral", "ttl": "1h"})
    markers = [d["cache_control"] for d in _walk_dicts(body) if "cache_control" in d]
    if any(m not in good for m in markers):
        found.append("bad_cache_control")
    if len(markers) > 4:
        found.append("too_many_cache_breakpoints")
    for key in sorted(set(_walk_keys(body)) & set(FORBIDDEN_KEYS)):
        found.append("forbidden_key:%s" % key)
    # The optional settings: at the top level, one member each, a known word.
    if "thinking" in body:
        thinking = body["thinking"]
        if not (isinstance(thinking, dict) and list(thinking) == ["type"]
                and thinking["type"] in THINKING_TYPES):
            found.append("bad_thinking")
    if any("thinking" in d for d in _walk_dicts(body) if d is not body):
        found.append("forbidden_key:thinking")
    if "output_config" in body:
        config = body["output_config"]
        if not (isinstance(config, dict) and list(config) == ["effort"] and config["effort"] in EFFORTS):
            found.append("bad_output_config")
    messages = body.get("messages")
    if not isinstance(messages, list) or not messages:
        found.append("missing_messages")
    else:
        roles = [m.get("role") if isinstance(m, dict) else None for m in messages]
        if any(r not in ("user", "assistant") for r in roles):
            found.append("bad_role")
        if roles[0] != "user":
            found.append("messages_not_starting_with_user")
        if any(a == b for a, b in zip(roles, roles[1:])):
            found.append("messages_not_alternating")
        for message in messages:
            content = message.get("content") if isinstance(message, dict) else None
            blocks = [{"type": "text", "text": content}] if isinstance(content, str) else content
            for block in blocks if isinstance(blocks, list) else []:
                if (isinstance(block, dict) and block.get("type") == "text"
                        and not (isinstance(block.get("text"), str) and block["text"].strip())):
                    if "empty_text_block" not in found:
                        found.append("empty_text_block")
    return found


# --------------------------------------------------------------------------------------------
# The server
# --------------------------------------------------------------------------------------------

class _ClientGone(Exception):
    """The client closed the connection (or the server is stopping) while the mock was writing."""

    def __init__(self, stopping: bool = False):
        Exception.__init__(self, "client gone")
        self.stopping = stopping


class _CloseRequest(Exception):
    def __init__(self, mode: str):
        Exception.__init__(self, mode)
        self.mode = mode


class _BadRequest(Exception):
    def __init__(self, status: int, message: str):
        Exception.__init__(self, message)
        self.status = status


class _Request:
    def __init__(self) -> None:
        self.method = ""
        self.target = ""
        self.headers: Dict[str, List[str]] = {}
        self.body = b""
        self.expect_100_sent = False
        self.t_headers_done = 0.0

    def first(self, name: str) -> Optional[str]:
        values = self.headers.get(name)
        return values[0] if values else None


class _Conn:
    """One accepted connection: buffered reads, writes that notice a vanished client."""

    def __init__(self, server: "MockLlm", sock: socket.socket):
        self.server = server
        self.sock = sock
        self.buf = bytearray()  # a bytearray: appending and trimming stay linear for big bodies
        self.reset_done = False

    # -- reading ---------------------------------------------------------------------------
    def _fill(self) -> None:
        data = self.sock.recv(65536)
        if not data:
            raise EOFError
        self.buf.extend(data)

    def _read_until(self, delim: bytes, limit: int) -> bytes:
        while True:
            at = self.buf.find(delim)
            if at >= 0:
                break
            if len(self.buf) > limit:
                raise _BadRequest(431, "header section too large")
            self._fill()
        head = bytes(self.buf[:at])
        del self.buf[:at + len(delim)]
        return head

    def _read_n(self, count: int) -> bytes:
        while len(self.buf) < count:
            self._fill()
        data = bytes(self.buf[:count])
        del self.buf[:count]
        return data

    def read_request(self) -> Optional[_Request]:
        try:
            head = self._read_until(b"\r\n\r\n", MAX_HEAD_BYTES)
        except EOFError:
            return None
        lines = head.decode("latin-1").split("\r\n")
        parts = lines[0].split(" ")
        if len(parts) != 3 or not parts[2].startswith("HTTP/"):
            raise _BadRequest(400, "malformed request line")
        req = _Request()
        req.method, req.target = parts[0], parts[1]
        for line in lines[1:]:
            name, sep, value = line.partition(":")
            if not sep:
                raise _BadRequest(400, "malformed header line")
            req.headers.setdefault(name.strip().lower(), []).append(value.strip())
        req.t_headers_done = self.server.now()
        chunked = "chunked" in (req.first("transfer-encoding") or "").lower()
        lengths = set(req.headers.get("content-length", []))
        if len(lengths) > 1:
            raise _BadRequest(400, "conflicting content-length headers")
        length_text = next(iter(lengths), None)
        if length_text is not None and not re.fullmatch(r"[0-9]{1,18}", length_text):
            raise _BadRequest(400, "bad content-length")
        length = int(length_text) if length_text is not None else 0
        if length > MAX_BODY_BYTES:
            raise _BadRequest(413, "request too large")
        if (length or chunked) and (req.first("expect") or "").lower() == "100-continue":
            self.send(b"HTTP/1.1 100 Continue\r\n\r\n")
            req.expect_100_sent = True
        try:
            req.body = self._read_chunked_body() if chunked else self._read_n(length)
        except EOFError:
            return None
        return req

    def _read_chunked_body(self) -> bytes:
        body = bytearray()
        while True:
            size_line = self._read_until(b"\r\n", 1024).split(b";", 1)[0].rstrip(b" \t")
            if not re.fullmatch(rb"[0-9a-fA-F]{1,16}", size_line):
                raise _BadRequest(400, "bad chunk size")
            size = int(size_line, 16)
            if len(body) + size > MAX_BODY_BYTES:  # refused before the chunk is read
                raise _BadRequest(413, "request too large")
            if size == 0:
                while self._read_until(b"\r\n", MAX_HEAD_BYTES):  # trailers up to the empty line
                    pass
                return bytes(body)
            body.extend(self._read_n(size))
            if self._read_n(2) != b"\r\n":
                raise _BadRequest(400, "chunk data not followed by CRLF")

    # -- writing and disconnect detection --------------------------------------------------
    def gone(self) -> bool:
        """True if the client closed (a FIN or an RST), found by a non-blocking peek."""
        try:
            readable, _, _ = select.select([self.sock], [], [], 0)
        except (OSError, ValueError):
            return True
        if not readable:
            return False
        try:
            return self.sock.recv(1, socket.MSG_PEEK) == b""
        except (BlockingIOError, socket.timeout):
            return False
        except OSError:
            return True

    def send(self, data: bytes) -> None:
        if self.server.stopping():
            raise _ClientGone(stopping=True)
        if self.gone():
            raise _ClientGone()
        try:
            self.sock.sendall(data)
        except (OSError, ValueError):  # BrokenPipe, ConnectionReset, ConnectionAborted, timeouts
            raise _ClientGone()

    def sleep(self, seconds: float, watch: bool = True) -> None:
        """Waits, in short slices, and gives up early when the client goes away."""
        end = time.monotonic() + seconds
        while True:
            remaining = end - time.monotonic()
            if remaining <= 0:
                return
            if self.server.wait_stop(min(remaining, 0.02)):
                raise _ClientGone(stopping=True)
            if watch and self.gone():
                raise _ClientGone()

    # -- closing ---------------------------------------------------------------------------
    def finish(self) -> None:
        """A normal close: FIN, wait a moment for the client's own close, then close."""
        if self.reset_done:
            return
        try:
            self.sock.shutdown(socket.SHUT_WR)
            self.sock.settimeout(0.3)
            for _ in range(20):
                if not self.sock.recv(4096):
                    break
        except OSError:
            pass
        self.close()

    def reset(self) -> None:
        """Closes with SO_LINGER 0, which makes the stack send an RST instead of a FIN."""
        try:
            fmt = "HH" if sys.platform == "win32" else "ii"
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack(fmt, 1, 0))
        except OSError:
            pass
        self.reset_done = True
        self.close()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class MockLlm:
    """The mock server. ``scenarios`` is a queue of builtin names, scenario dicts or JSON file paths."""

    def __init__(self, scenarios: Optional[Sequence[Any]] = None, host: str = "127.0.0.1", port: int = 0,
                 default: str = "basic", decoy_url: str = DEFAULT_DECOY_URL, min_cacheable_tokens: int = 512,
                 cache_ttl: float = 300.0, record_dir: Optional[str] = None, require_auth: bool = False,
                 io_timeout: float = 30.0, stall_seconds: float = 120.0,
                 clock: Callable[[], float] = time.monotonic, verbose: bool = False):
        if host not in LOOPBACK_HOSTS:
            raise ValueError("host must be one of %s (the mock never listens beyond loopback)" % ", ".join(LOOPBACK_HOSTS))
        if default not in BUILTINS:
            raise ValueError("unknown default scenario %r" % default)
        self.host = host
        self.port = port
        self.default = default
        self.decoy_url = decoy_url
        self.record_dir = record_dir
        self.require_auth = require_auth
        self.io_timeout = io_timeout
        self.stall_seconds = stall_seconds
        self.verbose = verbose
        self.cache = CacheEmulator(min_cacheable_tokens, cache_ttl, clock)
        self.quit_event = threading.Event()
        self._named: Dict[str, Any] = {}
        self._queue: List[Tuple[str, Any]] = []
        self._records: List[Dict[str, Any]] = []
        self._cond = threading.Condition()
        self._secrets: Dict[str, str] = {}
        self._secret_lock = threading.Lock()
        self._stop = threading.Event()
        self._lsock: Optional[socket.socket] = None
        self._accept_thread: Optional[threading.Thread] = None
        self._threads: List[threading.Thread] = []
        self._socks: List[socket.socket] = []
        self._t0 = time.monotonic()
        for item in scenarios or ():
            self.add_scenario(item)

    # -- lifecycle -------------------------------------------------------------------------
    @property
    def url(self) -> str:
        host = "[%s]" % self.host if ":" in self.host else self.host
        return "http://%s:%d" % (host, self.port)

    def start(self) -> "MockLlm":
        family = socket.AF_INET6 if ":" in self.host else socket.AF_INET
        sock = socket.socket(family, socket.SOCK_STREAM)
        try:
            sock.bind((self.host, self.port))  # port 0: the OS picks, no pick-then-bind race
            sock.listen(64)
            sock.settimeout(0.05)
        except OSError:
            sock.close()
            raise
        self._lsock = sock
        self.port = sock.getsockname()[1]
        self._t0 = time.monotonic()
        self._accept_thread = threading.Thread(target=self._accept_loop, name="mock-llm-accept", daemon=True)
        self._accept_thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        if self._lsock is not None:
            try:
                self._lsock.close()
            except OSError:
                pass
            self._lsock = None
        with self._cond:
            socks, threads = list(self._socks), list(self._threads)
        for sock in socks:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        for thread in [self._accept_thread] + threads:
            if thread is not None and thread is not threading.current_thread():
                thread.join(3.0)
        with self._cond:
            self._cond.notify_all()

    def __enter__(self) -> "MockLlm":
        return self.start()

    def __exit__(self, *exc: Any) -> None:
        self.stop()

    def stopping(self) -> bool:
        return self._stop.is_set()

    def wait_stop(self, seconds: float) -> bool:
        return self._stop.wait(seconds)

    def now(self) -> float:
        return time.monotonic() - self._t0

    # -- scenarios -------------------------------------------------------------------------
    def _coerce(self, item: Any) -> List[Tuple[str, Any]]:
        if isinstance(item, str):
            if item in BUILTINS:
                return [(item, BUILTINS[item])]
            base, _, arg = item.partition(":")
            if arg and base in BUILTINS and callable(BUILTINS[base]):
                return [(item, BUILTINS[base])]
            if item in self._named:
                return [(item, self._named[item])]
            if os.path.isfile(item):
                return [(sc["name"], sc) for sc in load_scenario_file(item)]
            raise ValueError("unknown scenario %r (not a builtin name or a file)" % item)
        validate_scenario(item)
        return [(item.get("name", "custom"), item)]

    def add_scenario(self, item: Any) -> None:
        """Appends to the queue; the scenario is also selectable by ``model: "mock:<name>"``."""
        entries = self._coerce(item)
        with self._cond:
            for name, source in entries:
                if not isinstance(item, str) or item not in BUILTINS:
                    self._named[name] = source
                self._queue.append((name, source))

    def set_scenarios(self, items: Sequence[Any]) -> None:
        """Replaces the queue."""
        entries = [e for item in items for e in self._coerce(item)]
        with self._cond:
            self._queue = entries

    def _select(self, body: Any) -> Tuple[str, Any]:
        model = body.get("model") if isinstance(body, dict) else None
        with self._cond:
            if isinstance(model, str) and model.startswith("mock:"):
                name = model[len("mock:"):]
                source = self._named.get(name, BUILTINS.get(name))
                if source is None and ":" in name:
                    source = BUILTINS.get(name.split(":", 1)[0])
                return (name, source) if source is not None else ("unknown:" + name, None)
            if len(self._queue) > 1:
                return self._queue.pop(0)  # the last one stays and repeats
            if self._queue:
                return self._queue[0]
        return (self.default, BUILTINS[self.default])

    # -- records and secrets ---------------------------------------------------------------
    @property
    def requests(self) -> List[Dict[str, Any]]:
        """A snapshot (deep copy) of the recorded requests, in arrival order."""
        with self._cond:
            return copy.deepcopy(self._records)

    def wait_for_requests(self, count: int, timeout: float = 10.0) -> bool:
        with self._cond:
            return self._cond.wait_for(lambda: len(self._records) >= count, timeout)

    def wait_closed(self, index: int, timeout: float = 10.0) -> bool:
        """True once request ``index`` is over: the client closed early, or the response is finished."""
        with self._cond:
            return self._cond.wait_for(
                lambda: len(self._records) > index and self._records[index]["t_close"] is not None, timeout)

    def _register_secret(self, value: str) -> None:
        if len(value) < 6:
            return
        digest = key_digest(value)
        with self._secret_lock:
            self._secrets[value] = digest
            escaped = json.dumps(value)[1:-1]
            if escaped != value:
                self._secrets[escaped] = digest

    def scrub(self, text: str) -> str:
        with self._secret_lock:
            items = sorted(self._secrets.items(), key=lambda kv: -len(kv[0]))
        for secret, digest in items:
            if secret in text:
                text = text.replace(secret, digest)
        return text

    def _scrub_obj(self, obj: Any) -> Any:
        if isinstance(obj, str):
            return self.scrub(obj)
        if isinstance(obj, list):
            return [self._scrub_obj(x) for x in obj]
        if isinstance(obj, dict):
            return {self.scrub(str(k)): self._scrub_obj(v) for k, v in obj.items()}
        return obj

    def _log(self, message: str) -> None:
        if self.verbose:
            sys.stderr.write(self.scrub(message) + "\n")
            sys.stderr.flush()

    def _update(self, rec: Dict[str, Any], **fields: Any) -> None:
        with self._cond:
            rec.update(fields)
            self._cond.notify_all()

    # -- accept loop and connection handling -----------------------------------------------
    def _accept_loop(self) -> None:
        while not self._stop.is_set():
            lsock = self._lsock
            if lsock is None:
                break
            try:
                sock, _ = lsock.accept()
            except socket.timeout:
                continue
            except OSError:
                if self._stop.is_set():
                    break
                time.sleep(0.01)
                continue
            thread = threading.Thread(target=self._handle, args=(sock, self.now()), name="mock-llm-conn", daemon=True)
            with self._cond:
                self._threads = [t for t in self._threads if t.is_alive()]
                self._threads.append(thread)
                self._socks.append(sock)
            thread.start()

    def _handle(self, sock: socket.socket, t_accept: float) -> None:
        conn = _Conn(self, sock)
        try:
            sock.settimeout(self.io_timeout)
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass
            try:
                req = conn.read_request()
            except _BadRequest as bad:
                self._simple(conn, bad.status, error_body("invalid_request_error", str(bad)))
                return
            if req is not None:
                self._serve(conn, req, t_accept)
        except (OSError, EOFError, _ClientGone):
            pass
        finally:
            conn.finish()
            with self._cond:
                if sock in self._socks:
                    self._socks.remove(sock)

    def _simple(self, conn: _Conn, status: int, body: Any, content_type: str = "application/json") -> None:
        """A short non-scenario response with a Content-Length body."""
        data = (body if isinstance(body, str) else _json(body)).encode("utf-8")
        head = ("HTTP/1.1 %d %s\r\ncontent-type: %s\r\ncontent-length: %d\r\nconnection: close\r\n\r\n"
                % (status, REASONS.get(status, "Mock Status"), content_type, len(data)))
        try:
            conn.send(head.encode("latin-1") + data)
        except _ClientGone:
            pass

    def _control(self, conn: _Conn, req: _Request) -> None:
        path = req.target.split("?", 1)[0]
        if path == "/__mock/requests" and req.method == "GET":
            text = self.scrub(json.dumps(self.requests, indent=2))
            self._simple(conn, 200, text)
        elif path == "/__mock/quit" and req.method == "POST":
            self._simple(conn, 200, {"ok": True})
            self.quit_event.set()
        else:
            self._simple(conn, 404, error_body("not_found_error", "unknown mock endpoint"))

    def _serve(self, conn: _Conn, req: _Request, t_accept: float) -> None:
        if req.target.split("?", 1)[0].startswith("/__mock/"):
            self._control(conn, req)
            return
        # Secrets first, so that nothing below can record or log them in clear text.
        key = ""
        for name in SENSITIVE_HEADERS:
            for value in req.headers.get(name, []):
                self._register_secret(value)
                token = value.split(" ", 1)[1].strip() if value.lower().startswith(("bearer ", "basic ")) else value
                self._register_secret(token)
                if name == "x-api-key" or (name == "authorization" and not key):
                    key = token if name == "authorization" else value
        body_json: Any = None
        try:
            body_json = json.loads(req.body.decode("utf-8")) if req.body else None
        except (ValueError, UnicodeDecodeError):
            body_json = None
        headers: Dict[str, Any] = {}
        for name, values in req.headers.items():
            shown = [key_digest(v) if name in SENSITIVE_HEADERS else self.scrub(v) for v in values]
            headers[name] = shown[0] if len(shown) == 1 else shown
        route = self._route(req.method, req.target)
        name, source = self._select(body_json) if route == "messages" else (route, None)
        auth_missing = self.require_auth and "x-api-key" not in req.headers and "authorization" not in req.headers
        if auth_missing:
            name = "auth_required"
        rec: Dict[str, Any] = {
            "index": None, "method": req.method, "path": self.scrub(req.target), "headers": headers,
            "body_json": self._scrub_obj(body_json), "body_bytes_len": len(req.body),
            "body_sha256": hashlib.sha256(req.body).hexdigest(), "scenario": name,
            "t_accept": round(t_accept, 6), "t_headers_done": round(req.t_headers_done, 6), "t_close": None,
            "client_closed_early": False, "events_sent": 0,
            "violations": validate_request(req.method, req.target, req.headers, body_json),
            "response_status": None, "cache": None, "expect_100_sent": req.expect_100_sent,
            "thinking_type": self._scrub_obj(request_setting(body_json, "thinking", "type")),
            "effort": self._scrub_obj(request_setting(body_json, "output_config", "effort")),
        }
        with self._cond:
            rec["index"] = len(self._records)
            self._records.append(rec)
            self._cond.notify_all()
        self._log("request %d %s %s -> %s" % (rec["index"], req.method, req.target, name))
        try:
            if auth_missing:
                self._respond(conn, rec, resolve_scenario(BUILTINS["http_401"], RenderContext()))
            elif route != "messages":
                self._respond(conn, rec, self._plain_route(route, req.method))
            elif source is None:
                message = "unknown mock scenario: " + name[len("unknown:"):]
                self._respond(conn, rec, {"name": name, "status": 404, "headers": {}, "framing": "chunked",
                                          "steps": [{"text": error_body("not_found_error", message)}]})
            else:
                def cache_fn() -> Dict[str, int]:
                    usage = self.cache.usage(body_json)
                    self._update(rec, cache=usage)
                    return usage
                arg = name.split(":", 1)[1] if ":" in name else None
                ctx = RenderContext(body_json, key, self.decoy_url, cache_fn, arg)
                try:
                    scenario = resolve_scenario(source, ctx, name)
                except ValueError as bad:
                    scenario = {"name": name, "status": 500, "headers": {}, "framing": "chunked", "steps": [
                        {"text": error_body("api_error", "mock scenario error: %s" % bad)}]}
                self._respond(conn, rec, scenario)
        except _ClientGone as gone:
            if not gone.stopping:
                self._update(rec, client_closed_early=True)
        finally:
            self._update(rec, t_close=round(self.now(), 6))
            self._write_record(rec)

    @staticmethod
    def _route(method: str, target: str) -> str:
        """Which endpoint a request is for; only a POST of /v1/messages plays a scenario."""
        path = target.split("?", 1)[0]
        if path == "/v1/messages":
            return "messages" if method == "POST" else "method_not_allowed"
        if path == "/v1/models":
            return "models" if method in ("GET", "HEAD") else "method_not_allowed"
        return "not_found"

    @staticmethod
    def _plain_route(route: str, method: str) -> Dict[str, Any]:
        """A fixed answer for the other routes. These never consume the scenario queue."""
        headers: Dict[str, str] = {}
        if route == "models":
            status, headers = 200, {"content-type": "application/json"}
            body: Any = {"data": [{"type": "model", "id": MODEL, "display_name": "Mock Model One",
                                   "created_at": "2025-01-01T00:00:00Z"}],
                         "has_more": False, "first_id": MODEL, "last_id": MODEL}
        elif route == "method_not_allowed":
            status, headers = 405, {"allow": "POST"}
            body = error_body("invalid_request_error", "method not allowed")
        else:
            status = 404
            body = error_body("not_found_error", "unknown path")
        # HEAD gets the same status and headers and no body at all (close framing, no steps).
        if method == "HEAD":
            return {"name": route, "status": status, "headers": headers, "framing": "close", "steps": []}
        return {"name": route, "status": status, "headers": headers, "framing": "chunked",
                "steps": [{"text": body}]}

    def _write_record(self, rec: Dict[str, Any]) -> None:
        if not self.record_dir:
            return
        with self._cond:
            text = self.scrub(json.dumps(rec, indent=2))
        try:
            os.makedirs(self.record_dir, exist_ok=True)
            path = os.path.join(self.record_dir, "request-%04d.json" % rec["index"])
            with open(path, "wb") as handle:
                handle.write(text.encode("utf-8"))
        except OSError:
            pass

    # -- playing a scenario ----------------------------------------------------------------
    def _respond(self, conn: _Conn, rec: Dict[str, Any], scenario: Dict[str, Any]) -> None:
        status, framing = scenario["status"], scenario["framing"]
        headers = {
            "content-type": "text/event-stream; charset=utf-8" if status == 200 else "application/json",
            "cache-control": "no-cache",
            "request-id": "req_mock%04d" % (rec["index"] + 1),
        }
        headers.update({k: v for k, v in scenario["headers"].items()
                        if k not in ("transfer-encoding", "connection", "content-length")})
        if framing == "chunked":
            headers["transfer-encoding"] = "chunked"
        headers["connection"] = "close"
        lines = ["HTTP/1.1 %d %s" % (status, REASONS.get(status, "Mock Status"))]
        lines += ["%s: %s" % (k, re.sub(r"[\r\n]+", " ", v)) for k, v in headers.items()]
        self._update(rec, response_status=status)
        conn.send(("\r\n".join(lines) + "\r\n\r\n").encode("utf-8"))
        try:
            for step in scenario["steps"]:
                self._run_step(conn, rec, step, framing)
            mode = "clean"
        except _CloseRequest as close:
            mode = close.mode
        if mode == "reset":
            conn.reset()
        elif mode == "clean" and framing == "chunked":
            conn.send(b"0\r\n\r\n")

    def _run_step(self, conn: _Conn, rec: Dict[str, Any], step: Dict[str, Any], framing: str) -> None:
        if "loop" in step:
            end = time.monotonic() + step.get("max_ms", 120000) / 1000.0
            count, done = step.get("count"), 0
            inner = step["loop"]
            while True:
                self._run_step(conn, rec, inner, framing)
                done += 1
                if count is not None and done >= count:
                    return
                if time.monotonic() >= end:
                    return
                if not inner.get("delay_ms") and count is None:
                    conn.sleep(0.01)
        if step.get("delay_ms"):
            conn.sleep(step["delay_ms"] / 1000.0)
        if "hold_ms" in step:
            conn.sleep(step["hold_ms"] / 1000.0, watch=bool(step.get("until_client_close")))
        elif "stall" in step:
            conn.sleep(self.stall_seconds)
        elif "close" in step:
            raise _CloseRequest(step["close"])
        else:
            data = step_bytes(step)
            assert data is not None
            for number, piece in enumerate(split_pieces(data, step)):
                if number and step.get("piece_delay_ms"):
                    conn.sleep(step["piece_delay_ms"] / 1000.0)
                # One HTTP chunk per piece, written with a single send.
                conn.send(b"%x\r\n%s\r\n" % (len(piece), piece) if framing == "chunked" else piece)
            if "sse" in step:
                with self._cond:
                    rec["events_sent"] += 1


# --------------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Mock Anthropic Messages streaming server (synthetic scenarios, loopback only).")
    p.add_argument("--host", default="127.0.0.1", help="127.0.0.1 or ::1 (nothing else is accepted)")
    p.add_argument("--port", type=int, default=0, help="0 lets the OS choose; the port is printed on the LISTEN line")
    p.add_argument("--scenario", action="append", default=[], metavar="NAME|FILE",
                   help="queue a builtin scenario or a JSON file (repeatable; one per request, the last repeats)")
    p.add_argument("--default", default="basic", metavar="NAME", help="scenario used when the queue is empty")
    p.add_argument("--min-cacheable-tokens", type=int, default=512, help="cache emulator: smallest cacheable prefix")
    p.add_argument("--cache-ttl", type=float, default=300.0, help="cache emulator: entry lifetime in seconds")
    p.add_argument("--record-dir", help="write each finished request record here as request-NNNN.json")
    p.add_argument("--decoy-url", default=DEFAULT_DECOY_URL, help="the Location of the redirect_307 scenario")
    p.add_argument("--require-auth", action="store_true", help="answer 401 when no x-api-key or authorization came")
    p.add_argument("--ignore-stdin", action="store_true", help="do not exit when stdin closes")
    p.add_argument("--verbose", action="store_true", help="one stderr line per request (secrets scrubbed)")
    p.add_argument("--emit", metavar="NAME", help="print the scenario's SSE bytes (no HTTP framing) and exit")
    p.add_argument("--out", metavar="FILE", help="with --emit: write to FILE instead of stdout")
    p.add_argument("--write-fixtures", metavar="DIR", help="write every fixture scenario to DIR/<name>.sse and exit")
    p.add_argument("--list", action="store_true", help="list the builtin scenario names and exit")
    return p


def _write_binary(path: Optional[str], data: bytes) -> None:
    if path:
        with open(path, "wb") as handle:
            handle.write(data)
    else:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()


def _watch_stdin(event: threading.Event) -> None:
    # A raw os.read: a daemon thread blocked in a buffered stdin read can abort the interpreter at exit.
    try:
        if sys.stdin is not None:
            fd = sys.stdin.fileno()
            while os.read(fd, 4096):
                pass
    except (OSError, ValueError):
        pass
    event.set()


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.list:
        print("\n".join(builtin_names()))
        return 0
    if args.emit:
        if args.emit not in BUILTINS:
            parser.error("unknown scenario %r (see --list)" % args.emit)
        _write_binary(args.out, wire_body(args.emit))
        return 0
    if args.write_fixtures:
        os.makedirs(args.write_fixtures, exist_ok=True)
        for name in FIXTURE_NAMES:
            _write_binary(os.path.join(args.write_fixtures, name + ".sse"), wire_body(name))
        return 0
    if args.host not in LOOPBACK_HOSTS:
        parser.error("--host must be 127.0.0.1 or ::1; the mock listens on loopback only")
    if not 0 <= args.port <= 65535:
        parser.error("--port must be between 0 and 65535")
    try:
        mock = MockLlm(args.scenario, host=args.host, port=args.port, default=args.default,
                       decoy_url=args.decoy_url, min_cacheable_tokens=args.min_cacheable_tokens,
                       cache_ttl=args.cache_ttl, record_dir=args.record_dir, require_auth=args.require_auth,
                       verbose=args.verbose)
        mock.start()
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    stop = mock.quit_event

    def on_signal(_signum: int, _frame: Any) -> None:
        stop.set()

    for name in ("SIGTERM", "SIGINT", "SIGBREAK"):
        number = getattr(signal, name, None)
        if number is not None:
            try:
                signal.signal(number, on_signal)
            except (OSError, ValueError):
                pass
    if not args.ignore_stdin:
        threading.Thread(target=_watch_stdin, args=(stop,), name="mock-llm-stdin", daemon=True).start()
    host = "[%s]" % mock.host if ":" in mock.host else mock.host
    print("LISTEN %s:%d" % (host, mock.port), flush=True)
    try:
        while not stop.wait(0.25):
            pass
    except KeyboardInterrupt:
        pass
    finally:
        mock.stop()
        print("DONE", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
