# Curia architecture

Status: draft v2, 2026-10-04 (owner decisions of 2026-10-04 applied). Requirements: `requirements.md`. Evidence: `research-notes.md`. Items marked **(spike)** are hypotheses until the named test answers them.

## 1. Components

```
 ┌────────────── CK3 ──────────────┐            ┌──────────── Curia app (native, C++20) ────────────┐
 │  Curia mod                      │            │  log watcher ─► parser ─► SnapshotStore            │
 │  [Advisor] button (scripted     │  debug.log │        ▲                      │                   │
 │  widget) ─► scripted GUI effect ├───────────►│   file-change events          ▼                   │
 │  ─► debug_log "CURIA1|…" lines  │ (append)   │  UI thread (SDL3 + ImGui, event-driven)           │
 └─────────────────────────────────┘            │    overlay ◄─ chat model ◄─ PromptBuilder          │
                                                 │                │                                  │
                                                 │                ▼                                  │
                                                 │   Provider (interface) ─► libcurl net thread      │
                                                 │     ├ OpenAICompat   ├ AnthropicMessages          │
                                                 │     └ ClaudeAgent (post-v1, claude -p child)     │
                                                 │  platform layer: hotkey · secrets · window tweaks │
                                                 └───────────────────────────────────────────────────┘
```

The mod can't make network calls, so it only writes text. The app does everything else. v1 is one-way: nothing flows back into the game.

## 2. Data flow (happy path)

1. Player clicks **Advisor** in CK3. The scripted GUI effect (root = player) emits a framed export through `debug_log`.
2. CK3 appends marker lines to `logs/debug.log` (flush behaviour **spike T3**).
3. The watcher gets a file-change event, reads the appended bytes, hands lines to the parser. Non-marker lines are dropped.
4. When a complete frame arrives, the parser publishes a `Snapshot` to the UI thread (a custom SDL event). The overlay opens and shows "Snapshot: <in-game date>".
5. The player types or picks a question. `PromptBuilder` assembles `[system persona] + [snapshot] + conversation`, the selected `Provider` streams the answer; tokens arrive on the net thread and are posted to the UI thread as events.
6. Closing the overlay hides the window and returns focus to CK3. The app goes back to blocking in `SDL_WaitEvent`.

## 3. Trigger mechanism options

Decision D2: **log marker primary**. Clipboard stays an optional spike comparison (T9). Decision basis: SDL clipboard events don't fire in the background on Windows/macOS (C5); Wayland clipboard reads are focus-gated (research-notes §2.6); file reads are assumed not to be (A18, checked in T3).

| | A. Log marker (chosen) | B. Clipboard marker (VOTC-style, comparison only) |
|---|---|---|
| CK3 side | `debug_log` line with marker | needs a way to get text onto the clipboard from script/GUI (**not researched; VOTC's exact method not read**) |
| App side | file watcher + parser, which we need anyway for the payload | timer-poll `SDL_GetClipboardText` or native change counters; payload still arrives via log |
| Background behaviour | independent of focus | Win/macOS: `SDL_EVENT_CLIPBOARD_UPDATE` only on focus gain; GNOME Wayland: unreliable (C5, C7) |
| Latency | unknown: depends on `debug.log` flush (T3) | unknown (T9) |
| Risks | log flush delay; 17MB cap (C3); `debug_log` may need `-debug_mode` (T1) | overwrites the user's clipboard; same log dependency for data anyway |

If T3 shows long flush latency, the mitigation is protocol-level (frame with an end marker so partial data is never acted on; the UI shows "waiting for game…"), not a second trigger.

## 4. Mod design (`mod/curia/`)

- `.metadata/metadata.json` + `descriptor.mod` (T11 decides which the macOS launcher needs); `thumbnail.png`.
- `gui/curia_widget.gui`: window with the Advisor button. `gui/scripted_widgets/curia_widgets.txt`: registers it. No vanilla `.gui` is overridden (FIOS conflicts, research-notes §2.2).
- `common/scripted_guis/curia_*.txt`: scripted GUI (scope: character) whose `effect` calls the export effects.
- `common/scripted_effects/curia_export.txt`: the export, split per section (finance, succession, vassals, factions, wars, claims) so a section can be skipped to save log bytes.
- `localization/english/curia_l_english.yml`.
- Syntax is written against the game's own files and the owner's `script_docs` / `dump_data_types` output, not memory (section 9).

**Wire format (proposal, finalised after M0 T6 and M3 T4):** line-oriented, one `debug_log` call per line, pipe-separated, versioned prefix:

```
CURIA1|<snap_id>|BEGIN|<game_date>|<char_id>
CURIA1|<snap_id>|FIN|<gold>|<income>|…
CURIA1|<snap_id>|HEIR|…
CURIA1|<snap_id>|VASSAL|<rank>|<id>|<name>|<opinion>|…
CURIA1|<snap_id>|END|<line_count>
```

Rules: ASCII-safe field escaping defined once in `docs/wire-format.md` (to be written in M3); IDs not names where possible; numbers rounded; sections capped (top N vassals). The parser accepts a frame only if BEGIN…END match on `snap_id` and `line_count`. The engine's own line prefix (timestamps etc.) is stripped by locating the marker, not by assuming a layout (T4).

**M0 findings that shape the format (2026-10-04, `research-notes.md` §2.14):** every `debug_log` call is prefixed by the engine with time, level, source and `file: <script> line: <n> (<on_action/effect name>): `, about 110 bytes, so (1) the parser locates `CURIA1|` anywhere in the line and takes the text after it, (2) the exporter packs many fields per call, and the number of calls per export stays small (a single 8,000-character line survived intact in M0b), (3) values are written with inline bracket expressions in quoted strings (no loc files needed), using only forms proven in M0/M0b: from any scope `[THIS.GetCharacter.GetNameNoTooltip]` (or `THIS.Char`), `[THIS.GetCharacter.GetGold|0]`, `[THIS.Var('x').GetValue]`, `[THIS.ScriptValue('x')|0]`, `[THIS.GetCharacter.GetPrimaryTitle.GetNameNoTierNoTooltip]`, `[THIS.War.GetName]`, `[THIS.Title.GetNameNoTierNoTooltip]`; iterators `player_heir`, `ordered_vassal` (`max`, `order_by`), `every_character_war` (`primary_attacker`/`primary_defender`), `random_claim`; relational values (a vassal's or heir's opinion of the ruler) through a script value `"opinion(liege)"` (or `liege_or_court_owner`) read with `THIS.ScriptValue`, or precomputed into a variable inside the iterated scope; **avoid** `ROOT.*`, saved scopes (`sC`, `scope:`) and `scope:`-based script values (they come back empty and log script errors); use `NoTooltip` variants (plain `GetName` emits link markup); faction-scope forms are still to be pinned in M3; (4) the in-game date comes from a separate `debug_log_date` line or an exported value; (5) pipes survive.

**Log budget:** M3 T8 records bytes per export *and* total bytes appended to `debug.log` per hour of normal play. The app keeps a running total of all bytes appended to `debug.log` since the CK3 process started (reset when the log is recreated), a lower bound on the cumulative writes the cap counts (shared with `error.log`, not resettable by truncation; C3), and warns at a configurable threshold. The threshold has no default until T8; the 17MB figure is anecdotal.

## 5. App architecture

### 5.1 Threads
| Thread | Job |
|---|---|
| UI (main) | SDL event loop (`SDL_WaitEvent`), ImGui, renderer, tray, hotkey delivery. Never does I/O. Only thread that touches SDL (SDL clipboard and windowing are main-thread). |
| Watcher | blocks on the OS file-change primitive; reads appended bytes; parses; posts `SDL_PushEvent`. |
| Net | libcurl multi interface; `curl_multi_poll` + `curl_multi_wakeup` for commands; SSE parsing; posts token events. |
| Child-process reader | Agent Mode only (post-v1): reads the child's stdout lines, posts events. |

Idle behaviour: the overlay hidden ⇒ UI blocked, watcher blocked on the file event, net thread blocked in `curl_multi_poll` with no transfers. The ImGui frame loop renders only while visible and only when input or a stream event arrives (ImGui "event-driven" pattern: request a frame when something changed; one extra frame for animations settling).

### 5.2 Rendering and window (D1)
- `SDL_Renderer` (Direct3D 11 on Windows — never D3D12, which lacks transparency; Metal on macOS; OpenGL on Linux/X11) with `imgui_impl_sdl3` + `imgui_impl_sdlrenderer3` (vcpkg features `sdl3-binding` and `sdl3-renderer-binding`).
- Window flags: `SDL_WINDOW_TRANSPARENT | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP`, created hidden.
- The renderer sits behind `IRenderBackend` (init, new frame, render, shutdown) so SDL_GPU can replace it when SDL issue #15181 is resolved. Keep this seam tiny: ImGui backend calls only.
- Click-through: prefer a window sized to the chat panel (no transparent hit-test area to pass through). `SDL_SetWindowShape` only if the spike shows we need a fullscreen-sized overlay (click-through verified in source on macOS only).
- Positioning: find the CK3 window rect per OS (platform layer; macOS `CGWindowListCopyWindowInfo` for the game's pid (Apple does not document which keys need Screen Recording, so M2 checks whether bounds are populated without a prompt and never infers permissions from empty fields); Windows `FindWindow`/`GetWindowRect`; X11 EWMH). User can drag/scale; position persists.
- **macOS overlay recipe under test (M2; research-notes §2.5):** Accessory activation policy (set after `SDL_Init`, or `SDL_HINT_MAC_BACKGROUND_APP=1` before it, since SDL otherwise applies Regular); our own non-activating `NSPanel` subclass (`canBecomeKeyWindow` = YES, `hidesOnDeactivate` = NO) wrapped via `SDL_PROP_WINDOW_CREATE_COCOA_WINDOW_POINTER`, level `screenSaver` (1000), collection behaviour `canJoinAllSpaces | fullScreenAuxiliary | stationary` (a minimal variant of Apple's DTS sample, which also sets `canJoinAllApplications`; E3 adds it if nothing shows), shown with `orderFrontRegardless`; `SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN/RAISED` = 0; never call `SDL_SetWindowAlwaysOnTop/Resizable/Fullscreen` on it. Game-focus detection by `NSWorkspace` frontmost application (match the executable name `ck3`; its bundle id is empty), not by window scraping. Global hotkey: Carbon `RegisterEventHotKey` on the main thread, forwarded with `SDL_PushEvent`. All of it is unverified until M2 runs; nothing here is a result.
- Fallback ladder (FR-OVL-8; M2 records which rung works): overlay over Fullscreen → overlay with CK3 Windowed → normal companion window (e.g. second monitor). The app must be fully usable as a normal window.

### 5.3 Platform layer (`platform/{win,mac,linux}`, one interface)
```cpp
struct Platform {
  // window tweaks
  virtual void configureOverlay(SDL_Window*) = 0;      // level, collection behaviour, panel style
  virtual void showOverlay(SDL_Window*) = 0;           // remember frontmost app, order front
  virtual void hideOverlay(SDL_Window*) = 0;           // re-activate the remembered game window
  virtual std::optional<Rect> findGameWindow() = 0;
  // global hotkey (optional)
  virtual bool registerHotkey(Hotkey, std::function<void()> onFire) = 0;
  virtual void unregisterHotkey() = 0;
  // secrets
  virtual bool secretSet(std::string_view account, std::string_view secret) = 0;
  virtual std::optional<std::string> secretGet(std::string_view account) = 0;
  virtual bool secretDelete(std::string_view account) = 0;
  // paths
  virtual std::vector<std::filesystem::path> candidateCk3UserDirs() = 0;
  // child process (post-v1 Agent Mode): spawn with explicit env, no console window, kill tree
  virtual std::unique_ptr<ChildProcess> spawn(const SpawnSpec&) = 0;
};
```
| Concern | Windows | macOS | Linux |
|---|---|---|---|
| Overlay tweaks | topmost, `WS_EX_NOACTIVATE` toggling via `SDL_SetWindowFocusable` | Objective-C++: call `setActivationPolicy:Accessory` (or `LSUIElement`) and set `SDL_HINT_MAC_BACKGROUND_APP=1` before `SDL_Init` so SDL doesn't force Regular; non-activating `NSPanel`, level `screenSaver`, collection behaviour per spike | X11: `_NET_WM_STATE_ABOVE` via SDL; forced `SDL_VIDEO_DRIVER=x11` |
| Return focus | store HWND, `SetForegroundWindow` (restrictions apply; **spike**) | store `NSRunningApplication`, `activate` (**spike**) | `XSetInputFocus`/EWMH activate (best effort) |
| Hotkey | `RegisterHotKey` | Carbon `RegisterEventHotKey`; Cmd/Ctrl required | XGrabKey on X11; GlobalShortcuts portal (needs app id registration) |
| Secrets | Credential Manager | Keychain Services | libsecret; session-only memory fallback (D11) |
| User dir candidates | `Documents\Paradox Interactive\…` (use `SHGetKnownFolderPath`, not a hard-coded path: Documents can be redirected) | `~/Documents/Paradox Interactive/Crusader Kings III` | `~/.local/share/…`, `~/.paradoxinteractive/…`, Steam libraries' `compatdata/1158310/pfx/…` (read `libraryfolders.vdf`) |
| File watch | `ReadDirectoryChangesW` | kqueue (vnode `NOTE_EXTEND/WRITE`) or FSEvents (**spike T3**) | inotify |
| Spawn | `CreateProcess` + job object, `CREATE_NO_WINDOW` | `posix_spawn`, own process group | `posix_spawn`/`fork+exec`, own process group |

The three implementations are compiled per-OS in CMake; shared code never includes platform headers.

### 5.4 Core modules (`app/src/`)
- `core/`: app state machine, event types, settings, logging (app log never records keys).
- `bridge/`: `LogLocator`, `LogWatcher` (per-OS primitive behind one interface), `LineParser`, `FrameAssembler`, `SnapshotStore`, `LogBudget`.
- `advisor/`: `PromptBuilder`, persona text (embedded resource), starter questions.
- `providers/`: see 6.
- `ui/`: overlay window, chat view, settings view, theme/font (bundled; license recorded), `IRenderBackend`.
- `platform/`: see 5.3.

## 6. Providers
v1 ships `OpenAICompat` and `AnthropicMessages`; `ClaudeAgent` is post-v1 (M8) and kept here as design.

```cpp
enum class Role { System, User, Assistant };
struct Message { Role role; std::string text; };

struct CacheHints {            // honoured by AnthropicMessages; ignored elsewhere
  bool cacheSystem = true;     // breakpoint after the stable persona block
  bool cacheSnapshot = true;   // breakpoint after the realm snapshot block
  bool longTtl = false;        // 1 h instead of 5 min
};

struct ChatRequest {
  std::string model;                    // from models endpoint or free text (D4)
  std::string systemPersona;            // stable
  std::string snapshotText;             // changes per export
  std::vector<Message> conversation;    // user/assistant turns
  int maxTokens = 1024;
  bool showThinking = false;
  CacheHints cache;
  std::string resumeSessionId;          // Agent Mode only (post-v1)
};

enum class TokenKind { Text, Thinking };
struct Usage { int input=0, output=0, cacheRead=0, cacheWrite=0; std::optional<double> costUsd; };
struct ChatError { enum Code { Auth, RateLimit, Overloaded, Network, Cancelled, BadRequest, Policy, Other } code; std::string message; bool retryable; };

struct StreamHandlers {
  std::function<void(TokenKind, std::string_view)> onToken;
  std::function<void(const Usage&, std::string sessionId)> onDone;
  std::function<void(const ChatError&, std::string partialText)> onError;
};

class CancelToken { /* atomic flag + wakeup */ };

class Provider {
 public:
  virtual ~Provider() = default;
  virtual void streamChat(const ChatRequest&, StreamHandlers, CancelToken) = 0;   // async; handlers run on the net/reader thread
  virtual std::future<std::vector<ModelInfo>> listModels() = 0;                    // D4; may fail → free text
  virtual ProviderCaps caps() const = 0;                                            // supportsCaching, supportsThinking, supportsResume
};
```
(The brief's shorthand `streamChat(messages, onToken)` is the core; the request struct carries what caching and resume need. Final signatures settle in M4.)

### 6.1 OpenAICompat
`POST {base}/chat/completions` (`base` ends at `/v1`), `stream:true`, `stream_options.include_usage:true`. Auth header only when a key is set. Portable fields only (`model`, `messages`, `stream`, `stream_options`, `max_tokens`). Parser: `data:` lines, ignore `:` comments, `[DONE]`, empty/odd `choices`, `error` objects, reasoning keys `reasoning_content` | `reasoning` | `reasoning_details` → `TokenKind::Thinking`. Models: `GET {base}/models`. Presets (editable base URLs): OpenAI, OpenRouter, DeepSeek `https://api.deepseek.com`, Ollama `http://localhost:11434/v1`, LM Studio `http://localhost:1234/v1`, llama.cpp `http://127.0.0.1:8080/v1`.

### 6.2 AnthropicMessages
`POST https://api.anthropic.com/v1/messages`, `anthropic-version: 2023-06-01`, Bearer auth (legacy `x-api-key` fallback). Body: `system` as an array of text blocks: block 1 persona with `cache_control` (breakpoint); block 2 realm snapshot with its own breakpoint (so follow-up questions in the same snapshot hit the cache, a new snapshot only invalidates block 2 and later, since the order is tools → system → messages). Messages are the conversation. No `budget_tokens`, sampling params or forced tool choice (C13). Stream: `message_start` → `content_block_*` → `message_delta` (cumulative usage) → `message_stop`; `event: error` after 200; unknown events ignored; no `[DONE]`. Usage from `cache_creation_input_tokens`/`cache_read_input_tokens`. On a mid-stream error: keep partial text, offer "continue" which re-sends the partial text in a user message (prefill is unsupported on current models). Models: the models endpoint (A13).
Caching caveats: minimum cacheable prefix 512 tokens on current Sonnet/Opus/Fable, 4,096 on Haiku 4.5 — below it the request silently runs uncached; default TTL 5 min, optional 1 h (2× write cost) because game pauses can exceed 5 min. Hit rates must be measured (M4/M6a).

### 6.3 ClaudeAgent (post-v1, M8)
Spawn through `Platform::spawn` (candidate invocation from the 2026-10-04 research, to be validated by spikes S1–S10; `--restricted` may replace `--bare`):
```
claude -p --bare --output-format stream-json --verbose --include-partial-messages
       --append-system-prompt <persona> --tools "Read,Grep,Glob" --permission-mode dontAsk
       --permission-prompts none --disallowedTools "mcp__*"
       --settings '{"permissions":{"blockReadsOutsideWorkingDirectories":true}}'
       --max-turns <n> [--model <alias-or-id>] [--resume <session_id>]   # cwd = data dir, no --add-dir
```
(Final flag set verified against the installed version at runtime; `--permission-prompts` needs ≥2.1.259.) The user message (snapshot + question) goes via **stdin only**, never as an argument (visible in process listings), and no secret is ever passed on the command line. Environment: **explicit block** with `CLAUDE_CONFIG_DIR` pointing at a Curia-owned empty directory (FR-AGT-3). cwd: the Curia-owned data directory (non-secret snapshot history/notes only; never keys or config; no `.claude/` inside; FR-AGT-4). Confinement comes from cwd + `--tools` + `dontAsk` + `blockReadsOutsideWorkingDirectories`, not from allow/deny path rules (deny beats allow). Parse stdout lines: `system/init` → session id, `stream_event` text deltas → tokens, `result` → done/error (`is_error`, subtype). The app resends the snapshot each turn inside the prompt and passes `--append-system-prompt` on every spawn (whether `--resume` keeps the persona under `--bare` is unverified, M8); whether a resident `--input-format stream-json` process beats spawn-per-turn is a spike (M8). Model argument: the alias constant `sonnet` by default (the one whitelisted literal, FR-LLM-4), free text allowed. `claude auth status` is *not* used for gating, since `--bare` ignores login state. The app fails closed if `system/init` lists Bash or any tool other than Read/Grep/Glob.

Network: shared `HttpClient` wrapper: multi interface, one thread, `CURLOPT_NOSIGNAL=1`, `CURLOPT_LOW_SPEED_*` stall guard, `curl_multi_remove_handle` for cancel, CA handling per §8 (TLS).

## 7. Testing approach

CK3 cannot run in CI (and its files must not be redistributed), so everything is designed to be testable without it.

1. **Fake CK3 (`tools/fake_ck3/`, Python 3, no dependencies).** Creates a temp user folder with `logs/debug.log`; appends **synthetic** engine-style noise plus Curia frames on demand (CLI flags or a tiny stdin command) and can: line-buffer or delay flushes, truncate/recreate at "launch", write interleaved non-marker lines, emit partial frames, die after N bytes (simulating the cap), and run inside a Wine-like path layout. Until M3 produces real captures its format is a *hypothesis*; after M3 it matches the *structure* captured (prefix shape, interleaving, flush pattern). Rule: committed fixtures and the simulator contain only Curia marker lines plus hand-written synthetic noise; raw captures (engine lines, local paths) stay under gitignored `reference/` and are never copied into tracked files. A CI check greps tracked files for known engine-line patterns and user-path fragments.
2. **Parser/bridge unit tests** (doctest: MIT, fast to compile; Catch2 is the alternative): fixtures in `tests/fixtures/` derived from real game exports (our marker lines only, scrubbed; no engine lines, no Paradox-generated docs), plus malformed/fuzz inputs.
3. **Provider tests against a mock server** (`tools/mock_llm/`, Python): replays recorded Anthropic and OpenAI-style SSE streams including splits mid-line, mid-UTF-8, `ping`, comment lines, mid-stream errors, `[DONE]`. Agent Mode (post-v1) will be tested against a fake `claude` executable that prints recorded stream-json.
4. **End-to-end on every OS in CI:** fake CK3 → app (headless/offscreen smoke mode where windowing isn't available) → mock LLM → assert transcript. Windowing smoke tests only prove a window can be created and rendered on the runner (Linux uses Xvfb with `SDL_VIDEO_DRIVER=x11`); they say nothing about transparency, stacking or focus.
5. **CI** (GitHub Actions, RS-CI): matrix `windows-2022`, `macos-26` (arm64; plus a cross-built x86_64 slice merged with `lipo`, build-verified and Rosetta-smoke-tested only), `ubuntu-24.04` (images pinned); vcpkg manifest mode with a pinned `builtin-baseline`, release-only static overlay triplets, binary cache via `actions/cache` over the vcpkg `files` provider (vcpkg's `x-gha` backend no longer exists; NuGet on GitHub Packages is the upgrade path); jobs: build/test, lint (pinned `clang-format`, `clang-tidy` with vcpkg headers excluded), sanitizers (Linux, Clang, ASan+UBSan on the unit-test binary only), fuzz smoke (libFuzzer on the pure parser function with a committed seed corpus, fixed time budget, Linux only), mod structural lint, pattern scan (engine-line fragments, API-key shapes), and the aggregator **`CI result`** (`if: always()`, fails unless every need succeeded) as the only required check. Linux GUI-adjacent tests use `xvfb-run` or `SDL_VIDEODRIVER=dummy`. **No job handles Paradox content** (D6).
6. **Mod validation, local only:** ck3-tiger built with cargo on the owner's Mac against the local CK3 install (`--game <path>` if auto-detection fails, A17); `ck3-tiger.conf` in `mod/curia/`. Expect false positives while tiger targets 1.19.0 and the game is 1.20.0.3 (1.20 renamed religion and GUI items); keep a suppress baseline (`--suppress`) and review diffs on patch days. The game's `error.log` is the runtime check: after each in-game run, grep for `curia_` errors. A CI-safe structural lint of the mod (JSON validity of `metadata.json`, file name prefixes, line-format unit tests of the export spec) is allowed because it uses no game files.
7. **Human in-game tests** are listed per milestone in `milestones.md`; the owner covers macOS only.

## 8. Packaging and distribution
- **Mod:** Steam Workshop via the launcher upload flow (thumbnail 1:1 ≤1 MB, uploads start private, tags ≤5, e.g. Utilities); Paradox Mods publishing path to be confirmed in M7 (not researched).
- **App:** Windows: static-CRT zip (portable, no installer in v1; unsigned at first, SmartScreen instructions in the README). macOS: universal `.app` in a dmg (hardened runtime; Accessory app via `LSUIElement`); **interim** ad-hoc-signed un-notarized dmg with "Open Anyway" instructions until the Apple Developer account exists (D10), then Developer ID signed and notarized (`notarytool` with an App Store Connect API key, `stapler`). Linux: AppImage assembled by hand from the single static binary and built on the `ubuntu-24.04` runner (the floor is CK3's own, Ubuntu 24.04 / glibc 2.39; use an older container only if the floor is lowered), plus a `tar.gz`; no Flatpak (the sandbox complicates reading the Proton prefix, global shortcuts and the Secret Service).
- **TLS:** vcpkg's curl uses Schannel on Windows, Apple SecTrust on macOS and OpenSSL (default CA directory `/etc/ssl`) on Linux; M1 verified that libcurl's default CA handling works on macOS and Linux. The app uses the default and retries once with a probed system CA bundle (`net/ca_bundle.h`) only if the default fails with a CA error; Windows sets `CURLSSLOPT_REVOKE_BEST_EFFORT`. Verification is never disabled (A16).
- **Release process:** tag-triggered workflow, draft → upload → publish, `SHA256SUMS`, attestations, immutable releases (RS-REL).
- Versioning: app and mod carry the wire-format version (`CURIA1`); the app tells the user when the mod is older/newer than it understands.

## 9. Ground truth for CK3 script (workflow)
CK3 changes with patches; do not write script from memory.
1. Primary: the installed game's own files (on the owner's Mac: `~/Library/Application Support/Steam/steamapps/common/Crusader Kings III/game/…`, layout observed, research-notes §2.9). Read locally only.
2. Dumps: in a **throwaway game** (console use disables achievements for that game, research-notes C4/C20), launch with `-debug_mode`, open the console and run `script_docs` and `dump_data_types` (spelling per T10); outputs land in the user folder's `logs/`.
3. Copy dumps to `reference/ck3/` (gitignored). Never commit them, never paste them into docs or issues, never ship them (Paradox UGC clause covers only our own new content).
4. When writing a trigger/effect/GUI function, quote its entry from the local dump in the working notes, not from memory or web pages.

## 10. Repo layout (target)
```
curia/
  README.md  LICENSE (MIT)  CONTRIBUTING.md  CODE_OF_CONDUCT.md  SECURITY.md  SUPPORT.md
  CHANGELOG.md  THIRD_PARTY_NOTICES (generated)  .editorconfig  .clang-format  .clang-tidy  .gitattributes  .gitignore
  CMakeLists.txt  CMakePresets.json  vcpkg.json  cmake/ (triplets/, GenerateNotices.cmake)
  docs/               requirements · architecture · milestones · research-notes · spikes/ · wire-format.md (M3)
  mod/curia-m0/       throwaway M0 stub mod (not shipped)
  mod/curia/          the CK3 mod (descriptor, .metadata, gui, common, localization), MIT
  app/                src/{core,bridge,advisor,providers,ui,platform/{win,mac,linux}}, tests/, fuzz/, resources/ (font, persona)
  tools/fake_ck3/  tools/mock_llm/
  tests/fixtures/     Curia marker lines, recorded SSE streams (no Paradox-generated content, no raw engine lines)
  reference/          gitignored: local CK3 dumps and raw captures (never committed)
  .github/            CODEOWNERS · ISSUE_TEMPLATE/ · pull_request_template.md · dependabot.yml · release.yml · workflows/{ci,release,codeql,scorecard}.yml
```
