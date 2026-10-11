# Anthropic stream fixtures

Wire bytes of Anthropic Messages SSE streams for the provider tests of M4 (`SseParser`, the stream
decoder, the fuzz seed corpus). Each `<name>.sse` is **only the SSE body** (no HTTP framing,
nothing after the last byte), exactly what `tools/mock_llm` sends for the scenario of the same
name.

## Synthetic only

Everything here is made up: model `mock-model-1`, message id `msg_mock0001`, signature
`mock-signature-0001`, artificial text ("Test answer", "Test token 1."). The shapes follow the
documented stream (`docs/research-notes.md` section 2.7), not a capture. **Real API captures must
never be committed.** If you record real streams (for example with the app's `CURIA_RECORD_SSE`
debug option, once it exists), keep them under the gitignored `reference/` and, if a case is needed
here, rebuild it from the mock with artificial values.

## Generating

The files are generated, never edited by hand:

```
python3 tools/mock_llm/mock_llm.py --write-fixtures tests/fixtures/anthropic
python3 tools/mock_llm/mock_llm.py --emit NAME --out tests/fixtures/anthropic/NAME.sse   # one file
```

(`--out` instead of `>` so that Windows PowerShell does not re-encode the bytes.) The unit test
`tools/mock_llm/test_mock_llm.py` regenerates every fixture in memory and compares byte for byte,
and compares the output of `--emit` too, so a change to the mock that alters a stream fails CI until
the fixtures are regenerated in the same change. `.gitattributes` marks `*.sse` as `binary`: the
`crlf` fixture contains CR bytes that line-ending conversion must not touch.

## Files

Every stream is the standard sequence `message_start`, `content_block_start`, `content_block_delta`
(three text deltas: `Test answer`, ` from the`, ` mock server.`), `content_block_stop`,
`message_delta`, `message_stop`, with these differences:

| File | What differs |
|---|---|
| `basic.sse` | nothing: usage in `message_start` (`output_tokens: 1`) and cumulative in `message_delta` (25 in, 12 out) |
| `ping_interleaved.sse` | an `event: ping` after every event except the last |
| `unknown_event.sse` | an `event: mock_future_event` after `message_start` and a delta of type `mock_future_delta`; a client must ignore both |
| `split_every_byte.sse` | the logical stream of the scenario that the mock sends one byte per TCP send; the text `Test café costs €5 and \U0001F600 done.` holds 2-, 3- and 4-byte characters (the fixture is the byte stream; the splitting is a property of the scenario) |
| `crlf.sse` | all line endings are CRLF |
| `midstream_error.sse` | three text deltas, then `event: error` with `overloaded_error`; no `message_stop` (the HTTP status was 200) |
| `truncated.sse` | two text deltas, then the bytes stop (the scenario also ends the connection without the chunk terminator) |
| `thinking_blocks.sse` | a `thinking` block (two `thinking_delta`, one `signature_delta`) before the text block at index 1 |
| `refusal.sse` | text `Test refusal.`, `stop_reason: refusal` |
| `max_tokens.sse` | `stop_reason: max_tokens`, 4096 output tokens |
| `bad_json.sse` | the first `content_block_delta` has a `data:` line that is not JSON (`{not json`) |
| `usage_start_only.sse` | all four usage numbers (21 / 340 / 120 / 15) in `message_start`; `message_delta` has no `usage` |
| `usage_delta_only.sse` | `message_start` has no `usage`; `message_delta` holds all four |
| `usage_both.sse` | `message_start` holds the input and cache numbers with `output_tokens: 1`; `message_delta` holds the cumulative four |

The usage fields are `input_tokens`, `cache_creation_input_tokens`, `cache_read_input_tokens`,
`output_tokens`. The scenarios that need a live connection (HTTP errors, stall, reset, key echo,
cache emulation) have no fixture: they exist only in the running mock.

## Goldens

`<name>.decoded.json` is what `AnthropicStreamDecoder` must produce for `<name>.sse` (tokens, status
markers, the terminal call with usage or error, the accumulated text), with `showThinking` off. The
unit test decodes every `.sse` at chunk sizes 1, 2, 3, 7, 64 and whole and requires the same result
each time, then compares it with the golden (as parsed JSON, so the layout does not matter). To
rewrite them after a deliberate change: `CURIA_UPDATE_GOLDEN=1 ctest -R golden` (or run the
`curia_tests` binary directly with the variable set), then review the diff by hand against the table above. The same
variable rewrites the `request_*.json` files, which are the exact bytes `buildAnthropicRequestJson`
must give for a synthetic request (`request_basic.json`: persona and snapshot both cached;
`request_no_cache.json`; `request_ttl_1h.json`; `request_no_system.json`).
