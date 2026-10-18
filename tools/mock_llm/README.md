# Mock LLM

A mock Anthropic Messages streaming server, so that the provider code, the chat chain and the
secrecy checks can be tested without the real API (`docs/architecture.md` section 7, items 3 and
4). Python 3 standard library only (written for 3.9 to 3.14); it works on Windows, macOS and Linux
(raw sockets, threads, binary I/O, no `fork`, no `fcntl`, no `SO_REUSEADDR`).

It is a raw socket server, not `http.server`, because the tests need exact control: the mock owns
its listening socket (port 0, so the OS picks and nothing can race), runs one daemon thread per
connection with socket timeouts everywhere, and decides where each TCP send lands. It parses only
the request line, the headers and the body (`Content-Length`, or a chunked request body, and
`Expect: 100-continue` is answered). Responses use chunked transfer-encoding like the real API, or
`Connection: close` framing with no length.

Everything is synthetic: model `mock-model-1`, message id `msg_mock0001`, artificial text. **Real
API captures must never be committed**; they belong under the gitignored `reference/`.

## Command line

```
python3 tools/mock_llm/mock_llm.py [flags]
```

The first line printed is `LISTEN 127.0.0.1:<port>` (flushed); the last is `DONE`. The server
stops when stdin closes, on SIGTERM / SIGINT, or on `POST /__mock/quit` (the portable way, also on
Windows, where a terminated process cannot say `DONE`).

| Flag | Effect |
|---|---|
| `--host H` | `127.0.0.1` (default) or `::1`; anything else, `localhost` included, is refused |
| `--port N` | default 0: the OS chooses, read it from the `LISTEN` line |
| `--scenario NAME\|FILE` (repeatable) | queue a builtin scenario or a JSON file (one scenario object, or a list); one is consumed per request, the last repeats |
| `--default NAME` | scenario used when the queue is empty (default `basic`) |
| `--min-cacheable-tokens N`, `--cache-ttl S` | cache emulator (defaults 512 tokens, 300 s) |
| `--record-dir DIR` | write every finished request record as `request-NNNN.json` |
| `--decoy-url URL` | the `Location` of `redirect_307` (default a closed loopback port) |
| `--require-auth` | answer 401 when neither `x-api-key` nor `authorization` came (off by default) |
| `--ignore-stdin` | do not stop when stdin closes (for `&` and `nohup`, where stdin is `/dev/null`) |
| `--verbose` | one stderr line per request (method, path, scenario; secrets scrubbed) |
| `--emit NAME` [`--out FILE`] | print the scenario's exact wire body (the SSE stream only, no HTTP framing) in binary, or write it to FILE (use `--out` on Windows PowerShell, where `>` re-encodes) |
| `--write-fixtures DIR` | write `DIR/<name>.sse` for every fixture scenario |
| `--list` | list the builtin scenario names |

Scenario selection: a request whose JSON `model` starts with `mock:` plays the named scenario
(builtin, or one added by name; `mock:name:arg` and `--scenario name:arg` pass an argument to a builder that takes one, as `echo_key_split:20` does) and does not consume the queue; an unknown name is an HTTP 404 with
an Anthropic-style error body. Otherwise the queue is used.

## Scenarios

A scenario is a dict (or JSON file):

```json
{"name": "my_case", "status": 200, "headers": {"retry-after": "7"}, "framing": "chunked",
 "steps": [ ... ]}
```

`status` defaults to 200, `framing` is `chunked` (default) or `close`; `headers` are added to the
response (names are lowercased; `transfer-encoding`, `connection` and `content-length` are the
mock's own). Strings may contain `{key}` (the received key) and `{decoy_url}`. Steps run in order;
after the last one the connection ends cleanly.

| Step | Effect |
|---|---|
| `{"sse": {"event": "x" or null, "data": obj-or-string}}` | `event: x\ndata: <compact JSON>\n\n` (a string is sent as is, so it can be invalid JSON) |
| `{"raw": "text"}`, `{"b64": "..."}`, `{"text": str-or-obj}` | arbitrary bytes (UTF-8 text, base64, or text / compact JSON) |
| `{"gen": "answer", "text_parts": [...], "thinking": bool, "thinking_parts": [...], "usage": {...}, "usage_in": "start" \| "delta" \| "both", "stop_reason": "end_turn", "ping_every": n}` | one whole streamed answer as `sse` steps. `usage_in`: where the usage numbers appear. `start`: all in `message_start`, none in `message_delta`. `delta`: none in `message_start`, all in `message_delta`. `both` (default): input and cache numbers with `output_tokens: 1` in `message_start`, cumulative totals in `message_delta`. `ping_every: n` adds a `ping` after every n events |
| `{"hold_ms": N}` | idle for N ms, then the next step runs (the client closing is not watched) |
| `{"hold_ms": N, "until_client_close": true}` | idle for up to N ms; ends the request early when the client closes |
| `{"stall": true}` | send nothing more and keep the connection open until the client closes (120 s at most) |
| `{"close": "clean" \| "truncate" \| "reset"}` | end the connection: `clean` = the proper chunked terminator, `truncate` = FIN with no terminator, `reset` = `SO_LINGER 0`, the client sees an RST. Later steps are skipped |
| `{"loop": <data step>, "max_ms": 120000, "count": n}` | repeat a data step until the client closes (or `count` / `max_ms`) |

Options on any data step (a `gen` step passes them to every event it makes): `chunks: [sizes]`
(split into these sizes, the remainder follows), `every: k` (k bytes at a time),
`split_mid_utf8: true` (cut inside the first multi-byte character), `delay_ms` (wait before the
step), `piece_delay_ms` (wait between the pieces), `eol: "\r\n"` (line endings of `sse` / `gen`),
`ascii: true` (JSON with `\uXXXX` escapes). **Each piece is one HTTP chunk and one TCP send**, so
the HTTP chunk sizes a client decodes are exactly the pieces; TCP receive boundaries also show
when the pieces are paced with `piece_delay_ms`, but TCP may coalesce, so a test should rely on the
chunk sizes.

### Builtin scenarios (the names are the contract)

| Name | What it does |
|---|---|
| `basic` | one answer, three text deltas, usage in `message_start` and `message_delta` |
| `ping_interleaved` | a `ping` after every event |
| `unknown_event` | an event type and a delta type the client does not know |
| `split_every_byte` | 1-byte pieces; text with 2-, 3- and 4-byte UTF-8 characters, split mid-sequence |
| `crlf` | CRLF line endings |
| `midstream_error` | three tokens, then `event: error` `overloaded_error` after HTTP 200 |
| `truncated` | two tokens, then the connection ends without `message_stop` and without the chunk terminator |
| `thinking_blocks` | a thinking block first (`thinking_delta` and `signature_delta`), then the text block: the client reports "thinking" before the first text |
| `refusal`, `max_tokens` | `stop_reason` `refusal` / `max_tokens` |
| `bad_json` | one `data:` line that is not JSON |
| `usage_start_only`, `usage_delta_only`, `usage_both` | where usage and the cache fields appear |
| `http_400`, `http_401`, `http_403`, `http_404`, `http_413`, `http_429_retry_after` (`Retry-After: 7`), `http_500`, `http_529` | JSON error bodies `{"type":"error","error":{"type":...,"message":...}}` |
| `stall` | one token, then silence with the connection open |
| `slow_forever` | a token every 100 ms until the client disconnects |
| `hold_after_tokens` | three tokens, then idle until the client closes (the cancel test) |
| `reset_midstream` | three tokens, then an RST |
| `echo_key` | reflects the received key into a text delta and into an SSE `error` event (HTTP 200) |
| `echo_key_http` | reflects the received key into the message of an HTTP 401 error body |
| `redirect_307` | `307` with `Location: <decoy url>` |
| `cite` | reads the `system` text blocks for the gold figure and the heir's name and quotes them ("Gold is 312. Heir is Test Heir Two."), proving the realm block reached the request. It understands the app's rendered block, looking only between its markers `=== WHAT THE ADVISOR CAN SEE ===` and `=== END OF WHAT THE ADVISOR CAN SEE ===` (`Treasury: 312 gold ...`, `Heir: "Name"; ...`), answers `Nothing has been read yet.` to the block that says nothing has been read from the game yet, and, when there are no markers, reads plain `gold: N` / `heir: Name` lines. Its usage numbers come from the cache emulator, so a second request with the same system prefix reports `cache_read_input_tokens` |
| `echo_key_split` (`echo_key_split:N`) | streams the received key back in two `text_delta` events, cut after N characters of the key (default 12), and the first event's bytes also end on a TCP piece boundary right after the key's first part. Neither event holds the whole key: a client must redact the joined text. (The cut is found in the wire bytes, so a key with a quote or backslash, which JSON escapes, is not supported.) |
| `echo_key_half` (`echo_key_half:N`) | one `text_delta` with the first N characters of the received key (default 20), then silence until the client closes: a cancel leaves the key cut in two |
| `echo_key_half_close` (`echo_key_half_close:N`) | the same first part, then the connection is cut: a network error whose partial text holds half a key |
| `cache_demo` | like `basic`, usage from the cache emulator |
| `basic_close_framing`, `unicode_escaped` | `basic` with `Connection: close` framing; the UTF-8 text with `\u` escapes |

The deterministic ones are committed as wire bytes under `tests/fixtures/anthropic/` (see the
README there); `--emit` must reproduce them byte for byte, which the unit test checks.

## Cache emulator

For `cache_demo` (the builtin that takes its usage numbers from the emulator) the mock hashes the canonical
(sorted-keys JSON, `cache_control` removed) prefix up to each `cache_control` marker in the order
tools, system blocks, message content blocks (the first four markers count), estimates tokens as
`len(utf8) // 4`, and creates an entry only if the prefix has at least `--min-cacheable-tokens`
tokens. An entry lives `--cache-ttl` seconds (one hour for `ttl: "1h"`), and a read refreshes it.
First sight gives `cache_creation_input_tokens`, a repeat within the TTL gives
`cache_read_input_tokens` (a later marker with a changed suffix reads the longest cached prefix and
writes the rest), and `input_tokens` counts what follows the last marker. This is an approximation
of the documented behaviour, not the real tokenizer or the real eviction.

## Requests, records and key safety

Every request to a path outside `/__mock/` is recorded and checked, and a rule violation **never
rejects** it. Only a `POST` of `/v1/messages` (a query string is fine) plays a scenario and
consumes the queue; `GET`/`HEAD /v1/models` returns a synthetic list holding `mock-model-1`; any
other method on those two paths gets a `405`, any other path a `404` (Anthropic-style JSON error,
the queue untouched, the violation still recorded). The request framing is strict: a bad or
conflicting `Content-Length`, a bad chunk size or chunk data not followed by CRLF gets a `400`
(not recorded), and a body over 32 MiB a `413`, before it is read. A record holds `index`, `method`, `path`, `headers` (lowercased; a duplicated header is a list),
`body_json` (parsed, or null), `body_bytes_len`, `body_sha256`, `scenario`, `t_accept`,
`t_headers_done`, `t_close` (seconds since the server started), `client_closed_early`,
`events_sent`, `violations`, `response_status`, `cache`, `expect_100_sent`, and the request's
optional settings `thinking_type` (`thinking.type`) and `effort` (`output_config.effort`), each a
string or null (Curia without flags sends `effort` `low` and no thinking setting, D22; with
`--effort default` and no `--thinking`, both are null).

`violations` lists the broken rules of the request contract Curia's client must keep:
`wrong_method`, `wrong_path`, `missing_anthropic_version`, `missing_content_type`,
`bad_content_type`, `body_not_json`, `body_not_object`, `missing_model`, `missing_max_tokens`,
`stream_not_true`, `system_not_text_blocks`, `bad_cache_control` (anything but
`{"type":"ephemeral"}` or the same with `"ttl":"1h"`), `too_many_cache_breakpoints` (over four),
`forbidden_key:<name>` (`budget_tokens`, `temperature`, `top_p`, `top_k`, `tool_choice`,
anywhere in the body; `thinking` anywhere but at the top level), `bad_thinking` (a top-level
`thinking` that is not exactly `{"type": "adaptive" | "between_tools" | "disabled"}`),
`bad_output_config` (a top-level `output_config` that is not exactly `{"effort": "low" |
"medium" | "high" | "xhigh" | "max"}`), `missing_messages`, `bad_role`,
`messages_not_starting_with_user`, `messages_not_alternating`, `empty_text_block`.

`GET /__mock/requests` returns the records as JSON; `POST /__mock/quit` stops the server.

**The API key never leaves the mock in clear text.** The values of `authorization`, `x-api-key`
and `proxy-authorization`, and every other occurrence of the same string (or of the token after
`Bearer ` / `Basic `) in the path, headers, body, records, endpoint output, `--record-dir` files and
log lines, are replaced by `sha256:<first 12 hex>(len=<n>)`, which the library gives as
`key_digest(value)`. For a header the whole header value is digested (`key_digest("Bearer " + key)`).
The mock's stdout and stderr never print header values. The scrub is an exact string match against
the values it has seen in those three headers, so a secret that so far appeared only in a body or a
path is not masked until a header has carried it (and the `--record-dir` file of an earlier request
is already written). The one deliberate exception is the
*response* of `echo_key` / `echo_key_http`, which reflect the key so that a test can prove the
client under test redacts it. Test keys must be fake: build them at run time
(`"sk-" + "ant-" + "FAKE" + random tail`) so no tracked file holds a key-shaped string
(`tools/ci/scan_patterns.sh`).

Disconnects are found both by send errors (`BrokenPipe`, `ConnectionReset`, `ConnectionAborted`,
any `OSError`) and by a non-blocking `select` + `recv(MSG_PEEK)` that returns `b""`, so an idle
hold notices a closed client too. The record then has `client_closed_early: true` and a `t_close`.
A read-side EOF cannot be told apart from a full close, so a client that half-closes
(`shutdown(SHUT_WR)`) after its request counts as gone and gets no response: keep the write side open.

## Library

```python
import sys; sys.path.insert(0, "tools/mock_llm")
import mock_llm as ml

with ml.MockLlm(["http_529", "basic"], min_cacheable_tokens=512) as mock:
    url = mock.url                         # http://127.0.0.1:<port>
    # ... point the client under test at url, send two requests ...
    mock.wait_for_requests(2, timeout=10)
    mock.wait_closed(1, timeout=10)        # True: request 1 finished or the client closed it
    record = mock.requests[1]              # a deep copy of the record
    assert record["violations"] == []
    assert record["headers"]["x-api-key"] == ml.key_digest(my_fake_key)
    mock.add_scenario({"name": "mine", "steps": [{"gen": "answer", "every": 1}]})   # queued, and mock:mine
    mock.set_scenarios(["basic"])          # replace the queue
```

`MockLlm(scenarios=None, host="127.0.0.1", port=0, default="basic", decoy_url=..., min_cacheable_tokens=512,
cache_ttl=300.0, record_dir=None, require_auth=False, clock=time.monotonic, verbose=False)`;
`start()`, `stop()`, context manager, `url`, `host`, `port`, `requests`, `wait_for_requests(n, timeout)`,
`wait_closed(index, timeout)`, `add_scenario(x)`, `set_scenarios(list)`. Also `ml.wire_body(name)`,
`ml.builtin_names()`, `ml.key_digest(value)`, `ml.CacheEmulator`, `ml.validate_request(...)`.

## Tests

`python3 -m unittest discover -s tools/mock_llm` (CI job `lint`, step "Mock LLM unit tests") runs
`test_mock_llm.py`: every builtin scenario end to end with a raw-socket client written in the test
(no `urllib`, so proxy environment variables cannot interfere; the client decodes the chunked
bodies itself), split writes, disconnect and reset detection, key redaction (a fake key built at
run time must appear nowhere in the records, the endpoint, the record files or the output of a CLI
child, while `echo_key` reflects it), the queue and `mock:` selection, the cache emulator (a fake
clock replaces the TTL wait), each validation rule, loopback-only refusal, `Expect: 100-continue`,
the CLI lifecycle (`LISTEN`, quit endpoint, stdin close, SIGTERM, `DONE`) and `--emit` against
every committed fixture. Nothing synchronises on a fixed sleep; ports are OS chosen.

## How the C++ app uses it (planned, M4 PRs 4 and 6)

The provider test (`provider_mock`) and the chain test (`chat_e2e`) start this server as a child
(`--scenario ...`), read the `LISTEN` line, and run `curia --headless-chat --test-endpoint
http://127.0.0.1:<port>` with a key that contains `FAKE`. `--test-endpoint` is a command-line flag
only, compiled in only under `CURIA_ENABLE_DEV_ENV`, loopback literals only. Afterwards the test
reads `/__mock/requests`: no violations, `cite` quoted the fixture's gold and heir, a second
question shows `cache_read_input_tokens > 0` (`cache_demo`), a cancel shows `client_closed_early`
(`hold_after_tokens`), and the secrecy run proves the client redacted what `echo_key` reflected.
The test then posts `/__mock/quit` and expects `DONE`.

## Limits

- It matches the **documented** shape of the API (`docs/research-notes.md` section 2.7), not
  the live service: real rate-limit headers, real error bodies and the real cache come only from the
  owner's session against the real API.
- The cache emulator is an approximation (token estimate, no eviction pressure).
- `truncate` with `Connection: close` framing looks like a normal end to a client: use the default
  chunked framing to test truncation.
