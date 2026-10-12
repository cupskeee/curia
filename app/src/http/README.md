# curia_http

Everything in Curia that needs libcurl. It is a separate static library (`curia_http`, built only
with `CURIA_BUILD_APP`) so that `curia_core` stays free of curl and the sanitizer and fuzz presets
keep building without it.

| File | What it is |
|---|---|
| `http_client.*` | `HttpClient`: one net thread driving a `curl_multi` handle (`curl_multi_poll`, woken by `curl_multi_wakeup`). `Request` in, three callbacks out (`onHead`, `onData`, `onComplete`), all on the net thread. The one place that reads a `SecretString` (`reveal()`), to build the header line. |
| `tls_policy.*` | The CA policy shared by the client and `curia_https_smoke`: libcurl's default store first, one retry with a located system CA bundle after a certificate-trust failure. Verification is never switched off. |
| `header_capture.*` | Keeps only the status, `request-id` and `retry-after` from the response headers. Pure. |
| `transport_error.*` | libcurl result code to a plain sentence and a retry flag. Pure. |
| `anthropic_provider.*` | `Provider::streamChat` for the Messages API: `buildAnthropicRequest`, `SseParser`, `AnthropicStreamDecoder`, `mapHttpError`, on top of `HttpClient`. |

## Rules the client enforces

- HTTPS only, to the endpoint's own origin. A test endpoint (`--test-endpoint`, development builds)
  is plain http to a loopback literal, and no proxy is used for it. Environment proxies are honoured
  for the real host. The client does not trust the `Endpoint` it is given (any code can fill in that
  aggregate): `planTransfer()` accepts `isTest` only in a development build and only for the exact
  loopback URL `makeEndpoint()` builds (`net::isLoopbackTestUrl`), and a real endpoint only for
  Anthropic's own URL. A later provider for another service widens that rule there, on purpose.
- No redirects are followed (`FOLLOWLOCATION` off, `MAXREDIRS` 0): a redirect target could receive
  the key. A 3xx is reported as an error.
- The key travels only as a `SecretString` in a `Header`, and only when the request URL is exactly the
  endpoint's URL (`net::authMayAttach`); otherwise the request is refused and nothing is sent. The
  temporary header text is wiped after libcurl has copied it.
- Timeouts: connect 10 s (libcurl); stall guard 45 s: nothing received or sent for that long, measured
  with the client's own clock (restarted by every byte), so it fires after 45 s plus a few
  milliseconds. libcurl's `LOW_SPEED_*` is not used: it averages over a window and fires several
  seconds late. Tests shorten both through `ClientOptions`, never an environment variable.
- `Accept-Encoding: identity`: a compressed event stream can be held back by the compressor until a
  block fills, which would delay tokens; an answer is small.
- No verbose, debug or trace option and no verification switch is used anywhere
  (`tools/ci/check_source_rules.sh` fails on them).
- Cancel: the `CancelToken`'s waker wakes the net thread, which removes the transfer and closes the
  connection before it reports `End::Cancelled`. The waker is the same for every request of a client
  and stays on the token, so one token may be shared by several requests of one client. The
  destructor ends everything in flight the same way (`End::Shutdown`), joins the thread, and no
  callback runs after it returns.
- A callback may drop the last owner of the client or of the provider. The destructor then runs on
  the net thread, which cannot join itself: it hands the state to the net thread, which finishes the
  batch it is in, ends what is still in flight (`End::Shutdown` callbacks can run once more) and
  frees the state. The provider mutes its handlers first, so none of them runs after its destructor
  has started.
- Redaction: a token only loses a registered key that lies whole inside it (shapes are not applied
  to a piece: the front of a key that continues in the next token would be replaced alone and the
  rest of the key would stay in the clear). A key the server splits over two tokens passes as it
  is, so whoever joins the tokens into text (`ChatSession`) redacts the joined text with
  `redact()` and the final text with `redactFinal()`, which also cuts a key that the end of the
  stream split. The error message and the partial text of an error are redacted whole here.
- Memory is bounded: the client keeps no body (the consumer decides; the provider keeps the SSE
  parser's bounded buffers and at most 32 KiB of an error body).

## Tests

- `curia_http_tests` (label `unit`, `app/http_tests/`): TLS decisions, header parsing, transfer rules
  (also for a forged `Endpoint`), client lifecycle (cancel before start, cancel and destruction in
  flight with a silent loopback server, cancel latency from another thread, a shared token, the last
  owner dropped on the net thread, the stall guard on time, no `Expect:` header), provider edge
  cases. The folder is separate from `app/tests/` because that one is compiled by the core-only
  clang-tidy job of CI, which has no libcurl.
- Not covered yet: the TLS retry with a located CA bundle inside `HttpClient` (a test endpoint is
  plain http; the decisions are unit tested in `TlsPolicy`, the whole path only by `https_smoke`
  against a real host). A loopback TLS server with a throwaway certificate would cover it.
- `provider_mock` (labels `e2e`, `provider`; `app/tests/provider_mock.py`): the whole provider against
  `tools/mock_llm` through `curia_chat_probe`: every stream shape and HTTP status, stall, cancel with
  a socket-close check (the probe lingers after the request so that the process exit cannot be
  what closed the socket), key secrecy, no redirect, no proxy, the auth fallback, deterministic request
  bytes and the cache emulation.
- `curia_chat_probe` (`app/tools/chat_probe.cpp`) prints one JSON object per event; see its header.
- `chat_e2e` and `e2e_secrecy` (`app/tests/chat_e2e.py`): the whole chain through `curia --headless-chat` and `curia --check-api` (library `curia_headless`, `app/src/headless/`), with the key, redirect and proxy checks described in the file header.
- The provider reports the numbers of each connection (`AnthropicProviderOptions::onNetDone`: status, whether the CA fallback was used, connect and total times) for `--check-api`; it never carries text.
