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
2. CK3 appends marker lines to `logs/debug.log` (flush behaviour measured in M3 T3: no flush delay above 1.023 s seen over 2524 samples of CURIA lines (eight watcher runs; session 2 added 347 with a maximum of 0.956 s); engine stamps are whole seconds and one frame was seen 0.023 s after its second ended; non-CURIA lines were not examined for latency; `docs/spikes/m3-results.md`).
3. The watcher gets a file-change event, reads the appended bytes, hands lines to the parser. Non-marker lines are dropped.
4. When a complete frame arrives, the parser publishes a `Snapshot` to the UI thread (a custom SDL event). The overlay opens and shows "Updated from the game: <in-game date>" (plain words, FR-OVL-11).
5. The player types or picks a question. `PromptBuilder` assembles `[system persona] + [snapshot] + conversation`, the selected `Provider` streams the answer; tokens arrive on the net thread and are posted to the UI thread as events.
6. Closing the overlay hides the window and returns focus to CK3. The app goes back to blocking in `SDL_WaitEvent`.

## 3. Trigger mechanism options

Decision D2: **log marker primary**. Clipboard stays an optional spike comparison (T9). Decision basis: SDL clipboard events don't fire in the background on Windows/macOS (C5); Wayland clipboard reads are focus-gated (research-notes §2.6); file reads are assumed not to be (A18; not separately tested, T3 used a polling watcher in a separate process).

| | A. Log marker (chosen) | B. Clipboard marker (VOTC-style, comparison only) |
|---|---|---|
| CK3 side | `debug_log` line with marker | needs a way to get text onto the clipboard from script/GUI (**not researched; VOTC's exact method not read**) |
| App side | file watcher + parser, which we need anyway for the payload | timer-poll `SDL_GetClipboardText` or native change counters; payload still arrives via log |
| Background behaviour | independent of focus | Win/macOS: `SDL_EVENT_CLIPBOARD_UPDATE` only on focus gain; GNOME Wayland: unreliable (C5, C7) |
| Latency | measured (M3 T3, macOS): at most 1.023 s after the start of the engine's stamp second over 2524 samples (eight watcher runs; session 2 added 347 with a maximum of 0.956 s); sub-second not resolvable (`docs/spikes/m3-results.md`) | unknown (T9 not run) |
| Risks | 17MB cap (C3); `debug_log` does not need `-debug_mode` (M0 T1) | overwrites the user's clipboard; same log dependency for data anyway |

M3 T3 showed no long flush latency on the Mac; if a platform does, the mitigation is protocol-level (frame with an end marker so partial data is never acted on; the UI shows "waiting for game…"), not a second trigger.

## 4. Mod design (`mod/curia/`)

- `.metadata/metadata.json` + `descriptor.mod` (which one the macOS launcher needs is decided in M7, T11; both files are kept until then); `thumbnail.png`.
- `gui/curia_widget.gui`: window with the Advisor button. `gui/scripted_widgets/curia_widgets.txt`: registers it. No vanilla `.gui` is overridden (FIOS conflicts, research-notes §2.2).
- `common/scripted_guis/curia_*.txt`: scripted GUI (scope: character) whose `effect` calls the export effects.
- `common/scripted_effects/curia_export.txt`: the export, split per section (finance, succession, vassals, factions, wars, claims) so a section can be skipped to save log bytes.
- `localization/english/curia_l_english.yml`.
- Syntax is written against the game's own files and the owner's `script_docs` / `dump_data_types` output, not memory (section 9).

**Wire format (frozen 2026-10-11 as `CURIA1`; the specification is `docs/wire-format.md`):** line-oriented, one `debug_log` call per line, pipe-separated, versioned prefix. A shortened sample (the full line types, ranges and rules are in the spec):

```
CURIA1|<snap>|BEGIN|<mode>|<mod_version>|<year>|<month>|<day>|<ruler_id>|<ruler_name>
CURIA1|<snap>|FIN|<gold>|<net monthly balance in tenths of a gold piece>
CURIA1|<snap>|HEIR|<id>|<opinion>|<has_liege>|<landed>|<name>
CURIA1|<snap>|VASSALS|<total>|<listed>
CURIA1|<snap>|VASSAL|<rank>|<id>|<gold>|<opinion>|<strength>|<name>
…                       (WARS and WAR, CLAIMS and CLAIM, FACTIONS and FACTION groups)
CURIA1|<snap>|END|<record_count>
```

Rules (all in `docs/wire-format.md`): no escaping, at most one free-text field per record and it is always last, so a pipe or markup in a game string cannot move another field; IDs next to names so characters can be matched; numbers are integers rounded by the mod and treated as approximate (power ±1, income ±0.2 gold); sections capped (top 10 vassals, 10 explicit claims, 5 factions against the ruler); new data only as a new line type or sub-kind. The parser accepts a frame only if BEGIN…END match on the snapshot number and the record count (the count includes BEGIN, END and unknown types) and the section and group counts hold. The engine's own line prefix (timestamps etc.) is stripped by locating the marker, not by assuming a layout (T4).

**M0 findings that shape the format (2026-10-04, `research-notes.md` §2.14):** every `debug_log` call is prefixed by the engine with time, level, source and `file: <script> line: <n> (<on_action/effect name>): `, about 110 bytes in M0 and 119 to 124 bytes with the real mod's longer script path and effect names (M3 T4), so (1) the parser locates `CURIA1|` anywhere in the line and takes the text after it, (2) the exporter packs many fields per call, and the number of calls per export stays small (a single 8,000-character line survived intact in M0b), (3) values are written with inline bracket expressions in quoted strings (no loc files needed), using only forms proven in M0/M0b: from any scope `[THIS.GetCharacter.GetNameNoTooltip]` (or `THIS.Char`), `[THIS.GetCharacter.GetGold|0]`, `[THIS.Var('x').GetValue]`, `[THIS.ScriptValue('x')|0]`, `[THIS.GetCharacter.GetPrimaryTitle.GetNameNoTierNoTooltip]`, `[THIS.War.GetName]`, `[THIS.Title.GetNameNoTierNoTooltip]`; iterators `player_heir`, `ordered_vassal` (`max`, `order_by`), `every_character_war` (`primary_attacker`/`primary_defender`), `random_claim`; relational values (a vassal's or heir's opinion of the ruler) through a script value `"opinion(liege)"` (or `liege_or_court_owner`) read with `THIS.ScriptValue`, or precomputed into a variable inside the iterated scope; **avoid** `ROOT.*`, saved scopes (`sC`, `scope:`) and `scope:`-based script values (they come back empty and log script errors); use `NoTooltip` variants (plain `GetName` emits link markup); faction-scope forms (`THIS.Faction...`) did not run until M3 session 2 and then printed values with no error, but the faction name carries link markup even in the `NoTooltip` form (see the session 2 paragraph below); (4) the in-game date comes from a separate `debug_log_date` line or an exported value; (5) pipes survive.

**M3 findings that bear on the format (2026-10-10, `docs/spikes/m3-results.md`):** non-CURIA lines can sit inside a frame (multi-line engine error entries: 6 interleaved lines per frame in the run that also wrote an error per export, none in the re-check, whose exports were free of Curia errors), so the parser must tolerate them; `curia_snap_id` is a variable stored in the save (a save at 70 was followed by 71 after loading; a new game restarts at 1), and the `LOAD` line is written only at a new game, so a reload of a save is not announced; the END line count equalled all lines of the frame including BEGIN and END in every complete frame seen; the largest frame seen in session 1 and the re-check was 44 lines (the probe export; session 2 below saw 81) and no line appended during the watched windows exceeded 4000 bytes (content already present when a watcher attached was not counted). Proposals for the wire-format checkpoint are in the results file; none is decided.

**M3 session 2 findings that bear on the format (2026-10-11, `docs/spikes/m3-results.md`, `research-notes.md` §2.16, C35):** the faction-scope forms ran in three factions across two games (claimant, peasant, independence). The localized faction name carries link markup even in the `GetNameNoTooltip` form: control byte 0x15 delimits click and tooltip segments around the claimant and the title, and the markup contained `;`, `,` and `:` but no `|` in the one sample, so a name line is not plain text and the watcher flagged it as a control-byte anomaly; leader and target names (character `NoTooltip`) were clean. A `special_character` link and `Faction.GetSpecialCharacter` exist in the dumps for the claimant but were not run. Claims: the unfiltered claim counting equals the `explicit` = all, `pressed` = all filter; each of the two rulers' single claims was explicit (unpressed/weak in the Ironman game, pressed/strong in the throwaway save) and the UI lists both; the Ironman heir's three claims were implicit and count as pressed; no ruler with an implicit claim was seen. The largest frame seen is 81 lines (the probes export with two factions) and the longest CURIA line is 321 bytes including the engine prefix (the faction name line); the prefix was 119 to 131 bytes in session 2. A `LOAD` line may appear twice (two lines 40 s apart in one Ironman run, cause not established), and two factions with equal values give identical PROBE lines inside one frame, so a parser must not rely on one `LOAD` line or deduplicate lines in a frame by text. Which of these change the format is for the wire-format checkpoint; none is decided here.

**Log budget:** M3 recorded bytes per export (T8, `docs/spikes/m3-results.md`). Advisor: 18 lines, about 0.9 KB of text and 3.1 KB in the file (engine prefixes included) in a realm with 10 exported vassals; 7 lines, 230 B and 1,084 B in a realm with 1 vassal. Probes export: 44 and 31 lines in the same two realms; in session 2, 51 lines (a petty king) to 81 lines (two factions), 8.2 to 13.3 KB in the file, with the faction lines not capped. Total bytes appended to `debug.log` per hour of normal play are measured in normal play after M4. The app keeps a running total of all bytes appended to `debug.log` since the CK3 process started (reset when the log is truncated at launch; M3 T5 saw an in-place truncation, same inode), a lower bound on the cumulative writes the cap counts (shared with `error.log`, not resettable by truncation; C3), and warns at a configurable threshold. The threshold has no default in M3: it is chosen from the normal-play data by M6a, and until then the budget warning ships off/unset; the 17MB figure is anecdotal.

## 5. App architecture

### 5.1 Threads
| Thread | Job |
|---|---|
| UI (main) | SDL event loop (`SDL_WaitEvent`), ImGui, renderer, tray, hotkey delivery. Never does I/O. Only thread that touches SDL (SDL clipboard and windowing are main-thread). |
| Watcher | blocks on the OS file-change primitive; reads appended bytes; parses; posts `SDL_PushEvent`. |
| Net | libcurl multi interface; `curl_multi_poll` + `curl_multi_wakeup` for commands; SSE parsing; posts token events. |
| Child-process reader | Agent Mode only (post-v1): reads the child's stdout lines, posts events. |

Idle behaviour: the overlay hidden ⇒ UI blocked except for a once-a-second tick of the log-silence monitor (5.2a), watcher blocked on the file event, net thread blocked in `curl_multi_poll` with no transfers. The ImGui frame loop renders only while visible and only when input or a stream event arrives (ImGui "event-driven" pattern: request a frame when something changed; one extra frame for animations settling).

### 5.2 Rendering and window (D1)
- `SDL_Renderer` (Direct3D 11 on Windows — never D3D12, which lacks transparency; Metal on macOS; OpenGL on Linux/X11) with `imgui_impl_sdl3` + `imgui_impl_sdlrenderer3` (vcpkg features `sdl3-binding` and `sdl3-renderer-binding`).
- Window flags: `SDL_WINDOW_TRANSPARENT | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP`, created hidden.
- The renderer sits behind `IRenderBackend` (init, new frame, render, shutdown) so SDL_GPU can replace it when SDL issue #15181 is resolved. Keep this seam tiny: ImGui backend calls only.
- Click-through: prefer a window sized to the chat panel (no transparent hit-test area to pass through). `SDL_SetWindowShape` only if the spike shows we need a fullscreen-sized overlay (click-through verified in source on macOS only).
- Positioning: find the CK3 window rect per OS (platform layer; macOS `CGWindowListCopyWindowInfo` for the game's pid (verified in M2: bounds, layer and owner name come back populated with Screen Recording not granted and no prompt; window titles are empty; Apple does not document the gating, so never infer permissions from empty fields); Windows `FindWindow`/`GetWindowRect`; X11 EWMH). User can drag/scale; position persists.
- **macOS overlay recipe (verified visible in M2, Fullscreen; research-notes §2.5):** Accessory activation policy (set after `SDL_Init`; SDL otherwise applies Regular, and with `SDL_HINT_MAC_BACKGROUND_APP=1` set the default still read Regular in M2, so also set it explicitly or via `LSUIElement`); our own non-activating `NSPanel` subclass (`canBecomeKeyWindow` = YES, `hidesOnDeactivate` = NO) wrapped via `SDL_PROP_WINDOW_CREATE_COCOA_WINDOW_POINTER`, level `screenSaver` (1000), collection behaviour `canJoinAllSpaces | fullScreenAuxiliary | stationary` (a minimal variant of Apple's DTS sample, which also sets `canJoinAllApplications`; E3 covered both), shown with `orderFrontRegardless`; `SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN/RAISED` = 0; never call `SDL_SetWindowAlwaysOnTop/Resizable/Fullscreen` on it. No Accessibility API is ever called, not even `AXIsProcessTrusted()` (it made the system log an Accessibility request for the probe, research-notes §2.5). Game-focus detection by `NSWorkspace` frontmost application (match the executable name `ck3`; its bundle id is empty), not by window scraping. Global hotkey: Carbon `RegisterEventHotKey` on the main thread, forwarded with `SDL_PushEvent`. **Verified in M2 (owner observation, 2026-10-05, CK3 Fullscreen on macOS 26.5.1; `docs/spikes/m2-results.md`):** this panel was visible over CK3's native Space at all 20 level and flag combinations tried (levels 3, 25, 101, 1000), the plain SDL window was not (E1, E2). The real app starts from combination 1 (screen-saver level, `canJoinAllSpaces | fullScreenAuxiliary | stationary`); which element of the recipe is necessary was not isolated. Windowed: deferred to the M4 testing.
- **Placement (FR-OVL-9):** on every show, place the overlay on the display that contains CK3's window: the bounds of the `ck3` process's largest layer-0 window from `CGWindowListCopyWindowInfo` (bounds, layer and owner name are readable without Screen Recording; titles are empty). CK3's window is not the display: on the owner's Mac it was `0,39,2056,1290` on a `2056x1329` display, so use the window rectangle. The probe placed the panel once, on the display under the pointer, and never moved it; do not copy that.
- **Click-through:** whole-window click-through set through `ignoresMouseEvents` did not hold in M2 (the flag went back to 0 within seconds); the 3.4.18 source resets it on mouse-motion events for transparent windows unless a window shape is set (a source reading, not confirmed at runtime). The window is sized to the chat panel (no transparent hit area); `SDL_SetWindowShape` if per-pixel pass-through is ever needed (untested).
- **Tray:** secondary. The menu bar did not appear on CK3's Fullscreen display (two-display setup), and `SDL_CreateTray` activates the app unconditionally (the log shows it in the same millisecond); create it at start-up before CK3 is frontmost, never during play. Primary controls: the in-game button and the global hot key.
- **Focus:** the first click on the overlay made the app report itself active and the panel key (it received the keys) while NSWorkspace still reported CK3 frontmost and CK3 stayed in fullscreen; Esc returns the keys to CK3 with no click. The two sources disagree while the panel is key and the log cannot say which governs key routing: take "is CK3 in front" from NSWorkspace (a design choice).
- **Start-up policy:** the Info.plist sets `LSUIElement` and the app sets `SDL_HINT_MAC_BACKGROUND_APP` before `SDL_Init` (the probe ran in the Regular policy for about a millisecond at every start); M4 checks that no Dock icon appears.
- Fallback ladder (FR-OVL-8; M2 records which rung works): overlay over Fullscreen → overlay with CK3 Windowed → normal companion window (e.g. second monitor). The app must be fully usable as a normal window.

### 5.2a Chat view (M4; the companion window of FR-OVL-8 rung 3 until the overlay panel arrives)
The chat runs in the plain SDL window today (title "Curia", 520x680 logical pixels by default, resizable, at least 340x420; Windows and X11 count window sizes in pixels, so there the sizes are multiplied by the primary display's content scale, while macOS reports a content scale of 1; following a move to a display with another scale is still to do), drawn over the whole window with Dear ImGui (no inner ImGui window chrome). In a wide window the content is a centred column at most 29 font sizes wide (about 90 characters of an answer, as at the default width). The macOS overlay panel, the hot key, the tray, `Curia.app`, focus return and Esc-to-hide come later; this window stays usable as the companion-window fallback.

- **Pure parts, in `curia_core` (`app/src/chatview/`, unit-tested and in the clang-tidy scan):** `answer_text` (FR-OVL-12: `**bold**` and `__bold__` are rendered; single `*`/`_` markers are removed only when they clearly wrap a word, never inside a word such as snake_case or a name; `~~strikethrough~~` loses its markers, a single `~` stays; `# ` heading and `> ` quote markers removed, the line stays plain; `- ` and `* ` bullets become `• `; `1. ` numbered lines kept exactly; code backticks removed, code content untouched; `[text](url)` becomes its text; rules and code fences dropped; tables left as text; blank-line runs collapsed. While an answer streams, a marker at the very end is held back, an opening `**` makes the rest of its line bold until it closes, and a last line of markers only is not shown yet, so nothing flickers; once the answer is complete an unmatched `**` is dropped. The plain text for Copy is the shown text without markers); `text_layout` (word wrap over runs of plain and bold text with an injected measure function: breaks at blanks, keeps explicit and blank lines, breaks a word longer than the line between code points, keeps a bold/plain change inside a word, gives list lines a hanging indent; ImGui passes the font's own `CalcTextSizeA`); `view_model` (`buildChatView()`: from the controller's state, the status line, the notices, the text in the middle of an empty conversation (before the first update from the game: what to do, instead of a status line), the rows of the conversation with their state (thinking, streaming, complete, stopped, failed, the length-limit note), which buttons are on (Send, Stop, Retry on the last failed answer when the error is retryable, Copy on every answer that is no longer arriving and has text, including one stopped or failed part way), and whether something animates (the dot of "thinking..." and the one after an answer that is arriving). `shownAnswer()` parses an answer and checks the shown text for secrets once more: the session redacts the raw text, but a key the model split with markers (`sk-ant-**...**`, backticks, a link) would be joined again when the markers are removed, so such an answer is shown, and copied, redacted and as plain lines); `view_text` (every text the window shows of its own, checked against `advisor/plain_words.h` by `test_chatview_text.cpp`; the error summaries, the divider rows and the not-configured message are the advisor's own; the folder-not-found text names Documents › Paradox Interactive › Crusader Kings III on Windows and macOS and stays general on Linux); `log_path` (which `debug.log` the window reads); `canned_conversation` (the smoke test's script and a provider that plays it one piece per call).
- **ImGui parts, in the `curia` executable (`app/src/ui/`):** `chat_view` draws a `ChatView` (status and notices at the top, a warning with an accent bar on its left edge and an information without; when they would take more than 35% of the height above the input box they scroll in their own area; the conversation in a scrolling area that follows the newest text until the player scrolls up, with a thin rule along its top edge while it is scrolled down; the pause reminder right above the input box, its lead "Pause the game" in bold ink; a multi-line word-wrapping input with Send, or Stop while a question is in flight); `chat_window` is the controller's observer and carries out what the player did; `theme` sets the colours and sizes and loads the font; `sdl_wake` is the mailbox's wake-up (one `SDL_PushEvent` of a registered type, thread-safe, disarmed at shut-down); `smoke_run` is `--smoke`.
- **Keys:** Enter sends (`ImGuiInputTextFlags_CtrlEnterForNewLine`), Shift+Enter (and Cmd/Ctrl+Enter) adds a line; Cmd+. stops an answer on macOS (FR-OVL-4; ImGui's `ImGuiMod_Ctrl` is Cmd there with `io.ConfigMacOSXBehaviors`); Esc is taken from the input box so that it does not throw the typed question away (ImGui's text box reverts on Esc); hiding the overlay on Esc comes with the panel. The input has the keyboard focus when the window opens and after a click on Advisor in the game, never in `--idle-test`. Copy uses `SDL_SetClipboardText` on the UI thread.
- **Theme (FR-OVL-3):** parchment background, dark-brown ink, a deep red accent for buttons and labels, rounded but restrained frames; the bundled font is Gentium Book Plus 6.200 (SIL, OFL-1.1, Regular and Bold, unmodified, embedded in the executable at build time by `cmake/EmbedFile.cmake`; `app/resources/fonts/README.md`), 18 px times the display's content scale (the renderer applies the pixel density), lines 1.28 times the font size. ImGui hands the frame's rasterizer density (2 on a Retina display) only to the font that is pushed, so the bold face, which is measured and drawn without being pushed, is pushed and popped once per frame; otherwise it is baked at density 1 and scaled up. Contrast (WCAG 2 ratios, from `app/src/ui/theme.h`): ink on the parchment 11.2, on an answer 12.6, on a question 9.7, on the input 13.4, on a notice 10.0, on an information 10.5; the muted ink of the status line, hints and notes 6.2 on the parchment, 7.0 on an answer, 5.3 on a question, 5.5 on a notice, 5.8 on an information; the error colour on an answer 8.4; the accent on the parchment 8.5 and on a notice 7.5 (the bar of a warning); button text on the accent 9.5 (hovered 7.5, pressed 12.1); a disabled button's text 6.5; the frame of the input box 3.6 against the parchment and 4.3 against the input (WCAG 1.4.11 asks for 3). If the font cannot be loaded, ImGui's own font stands in and `--smoke` fails.
- **Run loop and pacing:** two registered event types, the mailbox's wake-up (it leads to `AppController::pump()`) and a tick once a second from an SDL timer (`AppController::onTick()`, which the log-silence monitor needs). Neither draws by itself: a frame is drawn after a real input or window event (two frames), after a change the controller reported to the window (a new word, a state change, a reading, a notice; two frames, the scroll position follows a frame later), or on the timeouts of `uiWaitPolicy()` (`core/ui_pacing.h`): every 33 ms while the thinking dot or the dot after an arriving answer pulses, every 250 ms while the input box has the keyboard (the caret blinks), otherwise `SDL_WaitEvent` blocks (the tick still wakes it once a second, also while the window is hidden). A hidden, minimised or covered window draws nothing; whether it is hidden is asked again after the events are handled, so that the frames of the event that showed, restored or uncovered it are drawn. A click on Advisor in the game raises the window (and restores it only if it is minimised, so that a maximised window keeps its size) and focuses the input (on macOS a normal window that is raised activates Curia, which leaves CK3's fullscreen Space; the panel will not). Shut-down order: the watcher stops, a request in flight is stopped, the wake-up is disarmed, then the controller and with it the provider (whose destructor joins the net thread). An exception that leaves the window is caught in `main()`, as in the headless modes: it prints only `curia: internal error`, never the exception's text, and exits 1.
- **The window mode's inputs:** the development configuration and the provider exactly as in `--headless-chat` (`--test-endpoint`, `--auth` and `--log-dir` work here too, with the same rules), or the not-configured message (Send is off, the realm status still works); the app log in its default folder (numbers and short words only); `CURIA_LOG_CONTENT=1` writes no transcript here (only `--headless-chat` does), which a note on stderr says. The game log is `--log <path>` if given, else the first `<candidateCk3UserDirs()>/logs/debug.log` that exists, else none: the status line then says that CK3's folder was not found and nothing is read. The path from the platform layer reaches the reader as a `std::filesystem::path` (never narrowed to a byte string, which on Windows fails for a user folder outside the ANSI code page). The reader attaches at the end of an existing file, so the window shows nothing about the realm until the player clicks Advisor after Curia started; a `--log` file that does not exist yet is reported missing once and read from its first byte when it appears; CK3 empties the log at launch, which the reader follows as a truncation.
- **`--smoke`:** deterministic, no environment variable, no CK3 folder, no network, no key: the real controller and window with the canned conversation (a synthetic reading as if the player had clicked Advisor; a question whose scripted answer streams in pieces of 9 bytes and uses bold, a heading, a numbered list of three actions, a bullet, inline code, a word too long for a line and names with diacritics and in Cyrillic; a second reading, which adds a divider row; a second question whose answer fails part way, with Retry). It checks that the conversation ended as scripted and that both faces have every character shown, then prints `CURIA_SMOKE_OK frames=N renderer=NAME ui_scale=S font=bundled glyphs_missing=0` (exit 1 and a `CURIA_SMOKE_FAIL` line otherwise). `--screenshot <file.bmp>` saves the last frame (read back before it is presented, `IRenderBackend::captureNextFrame()`) and `--window-size <W>x<H>` sets the window's size in logical pixels; both are accepted only with `--smoke`. The `app_smoke` test saves `app_smoke.bmp` in the build folder.
- **`--idle-test`:** the chat window with no provider and no log (deterministic), the tick timer running and the input not focused; frames after the quiet baseline must each be explained by a real event (the wake-up and the tick are internal and draw nothing here).

### 5.3 Platform layer (`platform/{win,mac,linux}`, one interface)
```cpp
struct Platform {
  // window tweaks
  virtual void configureOverlay(SDL_Window*) = 0;      // level, collection behaviour, panel style
  virtual void showOverlay(SDL_Window*) = 0;           // remember frontmost app, order front
  virtual void hideOverlay(SDL_Window*) = 0;           // re-activate the remembered game window
  virtual std::optional<Rect> findGameWindow() = 0;
  // global hotkey (primary, on by default)
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
| File watch | `ReadDirectoryChangesW` | kqueue (vnode `NOTE_EXTEND/WRITE`) or FSEvents (untested: the T3 watcher polled every 10 ms, A15; no notification test exists yet) | inotify |
| Spawn | `CreateProcess` + job object, `CREATE_NO_WINDOW` | `posix_spawn`, own process group | `posix_spawn`/`fork+exec`, own process group |

The three implementations are compiled per-OS in CMake; shared code never includes platform headers.

### 5.4 Core modules (`app/src/`)
- `core/`: app state machine, event types, settings, logging (app log never records keys).
- `bridge/`: `LogLocator`, `LogWatcher` (per-OS primitive behind one interface), `LineParser`, `FrameAssembler`, `LogBudget`.
- `advisor/`: the snapshot renderer, persona text (a raw string in `persona.cpp`), `PromptBuilder`, `SnapshotStore`, `ChatSession`, `LogSilenceMonitor`, starter questions (`app/src/advisor/README.md`); `core/` holds the `Mailbox`, `AppController` and `ui_pacing` that connect them to the threads.
- `providers/`: see 6.
- `chatview/`: the chat view's pure parts (answer formatting, word wrap, view model, the window's texts, the log path choice, the smoke script), in `curia_core` (5.2a).
- `ui/`: overlay window, chat view, settings view, theme/font (bundled; license recorded), `IRenderBackend` (5.2a).
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
  int maxTokens = 4096;                 // thinking tokens count toward it on current models (to be confirmed in M4); 1024 was too low
  Effort effort = Effort::Default;      // Default | Low | Medium | High | XHigh | Max; Default sends nothing; the app asks for kDefaultEffort = Low (D22)
  Thinking thinking = Thinking::Default; // Default | Adaptive | BetweenTools | Disabled; Default sends nothing (the app's choice, D22)
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
`POST https://api.anthropic.com/v1/messages`, `anthropic-version: 2023-06-01`, Bearer auth (legacy `x-api-key` fallback; M4's `curia --check-api --auth x-api-key` settles which one the API accepts for a key). The code is split so that everything except the libcurl transport is in `curia_core` (sanitized and fuzzed): `app/src/providers/` (SSE parser, stream decoder, request builder, error mapping), `app/src/advisor/` (snapshot renderer, persona, prompt builder, chat session, log-silence monitor) and `app/src/http/` (`curia_http`, the curl_multi net thread). M4 tests use `tools/mock_llm/` and a headless mode `curia --headless-chat` (the same log watcher, `AppController`, `ChatSession` and provider as the window will use; commands on stdin, one JSON object per event on stdout; the events, commands and exit codes are in `app/src/core/headless_chat.h`; the provider is injected, so the unit tests drive it with a fake; the Anthropic provider is attached in `app/src/headless/`). `curia --check-api [--cache-probe]` is the owner's preflight against the real service. Development transcripts (`CURIA_LOG_CONTENT=1`) are plain text files outside every source tree (`core/transcript_writer.h`). Body: `system` as an array of text blocks: block 1 persona with `cache_control` (breakpoint); block 2 realm snapshot with its own breakpoint (so follow-up questions in the same snapshot hit the cache, a new snapshot only invalidates block 2 and later, since the order is tools → system → messages). Block 2 is headed `=== WHAT THE ADVISOR CAN SEE ===` and ends with `=== END OF WHAT THE ADVISOR CAN SEE ===`; the persona refers to it by that name, and both are in plain words only, because the model repeats their wording to the player (FR-OVL-11, `app/src/advisor/plain_words.h`). Messages are the conversation. Optional settings (`curia --headless-chat` / `--check-api` flags `--effort` and `--thinking`): `output_config: {effort: low|medium|high|xhigh|max}` and `thinking: {type: adaptive|between_tools|disabled}`, each sent only when set and with that one member. The app's default (D22, from the owner's round-2 measurement) is effort `low` and no `thinking` field: the model still thinks adaptively, at a low effort (in round 2 on one question of five). The constant is `providers::kDefaultEffort`; the controller's options, the prompt settings and the command line's options start from it, while `ChatRequest` itself defaults to sending nothing. `--effort default` sends no effort setting, so the model runs its own default; `--thinking` alone comes with effort `low`. Which model accepts which value is left to the API (a 400), not to a model list in the app (D4). Thinking counts toward `max_tokens` and is billed as output; its text is not shown. No `budget_tokens`, sampling params or forced tool choice (C13). Stream: `message_start` → `content_block_*` → `message_delta` (cumulative usage) → `message_stop`; `event: error` after 200; unknown events ignored; no `[DONE]`. Usage from `cache_creation_input_tokens`/`cache_read_input_tokens`. On a mid-stream error: keep partial text, offer "continue" which re-sends the partial text in a user message (prefill is unsupported on current models). Models: the models endpoint (A13).
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

1. **Fake CK3 (`tools/fake_ck3/`, Python 3, no dependencies).** Creates a temp user folder with `logs/debug.log`; appends **synthetic** engine-style noise plus Curia frames on demand (CLI flags or a tiny stdin command) and can: line-buffer or delay flushes, truncate at "launch" (M3 T5 saw an in-place truncation to 0 bytes, same inode, not a new file), write interleaved non-marker lines, emit partial frames, die after N bytes (simulating the cap), and run inside a Wine-like path layout. M3 produced real captures (prefix shape, multi-line engine entries between frame lines in one run, no flush delay above 1.023 s seen (whole-second engine stamps; one frame was seen 0.023 s after its second ended); `docs/spikes/m3-results.md`); the simulator is to match that *structure*. Rule: committed fixtures and the simulator contain only Curia marker lines plus hand-written synthetic noise; raw captures (engine lines, local paths) stay under gitignored `reference/` and are never copied into tracked files. A CI check greps tracked files for known engine-line patterns and user-path fragments.
2. **Parser/bridge unit tests** (doctest: MIT, fast to compile; Catch2 is the alternative): fixtures in `tests/fixtures/` derived from real game exports (our marker lines only, scrubbed; no engine lines, no Paradox-generated docs), plus malformed/fuzz inputs.
3. **Provider tests against a mock server** (`tools/mock_llm/`, Python): replays recorded Anthropic and OpenAI-style SSE streams including splits mid-line, mid-UTF-8, `ping`, comment lines, mid-stream errors, `[DONE]`. Agent Mode (post-v1) will be tested against a fake `claude` executable that prints recorded stream-json.
4. **End-to-end on every OS in CI:** fake CK3 → app (headless/offscreen smoke mode where windowing isn't available) → mock LLM → assert transcript. Windowing smoke tests only prove a window can be created and rendered on the runner (Linux uses Xvfb with `SDL_VIDEO_DRIVER=x11`); they say nothing about transparency, stacking or focus.
5. **CI** (GitHub Actions, RS-CI): matrix `windows-2022`, `macos-26` (arm64; plus a cross-built x86_64 slice merged with `lipo`, build-verified and Rosetta-smoke-tested only), `ubuntu-24.04` (images pinned); vcpkg manifest mode with a pinned `builtin-baseline`, release-only static overlay triplets, binary cache via `actions/cache` over the vcpkg `files` provider (vcpkg's `x-gha` backend no longer exists; NuGet on GitHub Packages is the upgrade path); jobs: build/test, lint (pinned `clang-format`, `clang-tidy` with vcpkg headers excluded), sanitizers (Linux, Clang, ASan+UBSan on the unit-test binary only), fuzz smoke (libFuzzer on the pure parser function with a committed seed corpus, fixed time budget, Linux only), mod structural lint, pattern scan (engine-line fragments, API-key shapes), and the aggregator **`CI result`** (`if: always()`, fails unless every need succeeded) as the only required check. Linux GUI-adjacent tests use `xvfb-run` or `SDL_VIDEODRIVER=dummy`. **No job handles Paradox content** (D6).
6. **Mod validation, local only:** ck3-tiger built with cargo on the owner's Mac against the local CK3 install (`--game <path>` if auto-detection fails, A17); `ck3-tiger.conf` in `mod/curia/`. Expect false positives while tiger targets 1.19.0 and the game is 1.20.0.4 (1.20 renamed religion and GUI items); keep a suppress baseline (`--suppress`) and review diffs on patch days. The game's `error.log` is the runtime check: after each in-game run, grep for `curia_` errors. A CI-safe structural lint of the mod (JSON validity of `metadata.json`, file name prefixes, line-format unit tests of the export spec) is allowed because it uses no game files.
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
2. Dumps: in a **throwaway game** (console use disables achievements for that game, research-notes C4/C20), launch with the launcher's one-shot "Open game in Debug Mode" button (not a setting; the console was not available in a plain launch, one key press tested in M3 T10), open the console and run `script_docs` and `dump_data_types` (spelling per T10); outputs land in the user folder's `logs/`.
3. Copy dumps to `reference/ck3/` (gitignored). Never commit them, never paste them into docs or issues, never ship them (Paradox UGC clause covers only our own new content).
4. When writing a trigger/effect/GUI function, quote its entry from the local dump in the working notes, not from memory or web pages.

## 10. Repo layout (target)
```
curia/
  README.md  LICENSE (MIT)  CONTRIBUTING.md  CODE_OF_CONDUCT.md  SECURITY.md  SUPPORT.md
  CHANGELOG.md  THIRD_PARTY_NOTICES (generated)  .editorconfig  .clang-format  .clang-tidy  .gitattributes  .gitignore
  CMakeLists.txt  CMakePresets.json  vcpkg.json  cmake/ (triplets/, GenerateNotices.cmake, extra_notices.txt, EmbedFile.cmake)
  docs/               requirements · architecture · milestones · research-notes · spikes/ · wire-format.md (M3)
  mod/curia-m0/       throwaway M0 stub mod (not shipped)
  mod/curia/          the CK3 mod (descriptor, .metadata, gui, common, localization), MIT
  app/                src/{core,bridge,advisor,chatview,providers,http,net,headless,ui,platform/{win,mac,linux}}, tests/, fuzz/, resources/fonts/
  tools/fake_ck3/  tools/mock_llm/
  tests/fixtures/     Curia marker lines, recorded SSE streams (no Paradox-generated content, no raw engine lines)
  reference/          gitignored: local CK3 dumps and raw captures (never committed)
  .github/            CODEOWNERS · ISSUE_TEMPLATE/ · pull_request_template.md · dependabot.yml · release.yml · workflows/{ci,release,codeql,scorecard}.yml
```
