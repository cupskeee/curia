# Curia milestones

Status: draft v2, 2026-10-04 (owner decisions of 2026-10-04 applied; M2 order, scope and results updated 2026-10-05). Requirements: `requirements.md`. Design: `architecture.md`. Spike questions and evidence: `research-notes.md` (§5).

**Who tests what.** The owner can test on **macOS only** (arm64, macOS 26.5.1; CK3 1.20.0.3 installed locally). Legend:
- **[CI]** verified automatically on Windows + macOS + Linux.
- **[OWNER-MAC]** needs the owner, in game or on the Mac.
- **[TESTER]** needs a community tester (Windows, Linux, Proton, Intel Mac). The owner cannot verify these; they never block a milestone, but each is tracked as unverified until a tester reports (tester-report issue form, RS-TPL).
- **[LOCAL]** can be done and checked on a development machine without the game.
- **[MANUAL-GH]** a GitHub repository setting that cannot be committed as a file; the owner (repo admin) clicks it; the checklist is in M1.

Spike results are written to `docs/spikes/<name>.md`; answers are copied back into `research-notes.md` (move items from "spike" to "verified").

## Overview

| # | Milestone | Needs the game? | Owner test |
|---|---|---|---|
| 0 | In-game feasibility (stub mod, no app, no C++) | yes | OWNER-MAC |
| 1 | Skeleton + CI + repository standards | no | CI + MANUAL-GH |
| 2 | Overlay spike | CK3 on Mac (display modes) | OWNER-MAC; Windows/Linux TESTER |
| 3 | Mod spike | yes | OWNER-MAC |
| 4 | Vertical slice | yes | OWNER-MAC |
| 5 | Providers + settings | partly | OWNER-MAC; rest CI/TESTER |
| 6a | Full advisor (export + caching), **last v1 feature milestone** | yes | OWNER-MAC |
| 6b | Council personas (post-v1) | yes | OWNER-MAC |
| 7 | Packaging and publishing (v1.0 release) | partly | OWNER-MAC; Windows/Linux TESTER |
| 8 | Agent Mode (post-v1) | partly | OWNER-MAC |

Order: **M0 first and alone.** M0 gates all C++ (D9). After M0: M1 → M2 (time-boxed macOS overlay spike; owner decision 2026-10-05, originally M3 was first) → M3 (cheapest riskiest-first game checks) → M4 → M5 → M6a → M7. M2 and M3 are independent and can be swapped or interleaved.

---

## M0. In-game feasibility check (stub mod, no app)

Goal: prove, before any C++ or repo plumbing, that the whole approach is feasible in the real game. A throwaway-quality stub mod, no app: the owner reads `debug.log` by hand.

Tests (T-IDs in `research-notes.md` §5.1):
- **T0** Where is `logs/debug.log` on the owner's Mac, and does it exist after a default launch? (A listing of the owner's machine already shows the folder and file, §2.1; confirm in the owner's run.)
- **T1** Does `debug_log` write to `debug.log` on a default launch (no `-debug_mode`)? Does it also work in Ironman?
- **T2** Achievements: decide a safe observation method first; then test (a) Curia stub mod only, no `-debug_mode`; (b) `-debug_mode` with no console command; (c) after one console command (expected: blocked). **Step (c) and anything else using the console runs in a throwaway game only.**
- **T6** Do bracket expressions (e.g. scope/character-name text) and numeric values resolve inside `debug_log`, and how are unresolved ones rendered? **This gates the entire export design** (it decides whether the export can contain real values at all).

Work: [LOCAL] a minimal stub mod written from the game's own files (`mod/curia-m0/`, not the final mod): an event/on_action (no GUI needed) that calls `debug_log` with a marker, a value, and a bracket expression. [LOCAL] a one-page test protocol (`docs/spikes/m0-protocol.md`) with exact steps and a result table. [OWNER-MAC] runs it and reads `debug.log` by hand (a plain `grep CURIA` on the file).

Acceptance criteria:
1. T0, T1, T2, T6 each have a recorded answer with date, game version (1.20.0.3 or later) and OS; none estimated.
2. **Pass condition to proceed:** `debug_log` writes on a normal launch **or** the cost of `-debug_mode` (achievement impact) is known and accepted by the owner; and T6 shows values can be exported (or a workaround format is identified).
3. **If any test fails, stop and re-plan with the owner before any C++ is written (D9).** No M1 work starts until the owner confirms.
4. If T1/T2 pass: "achievement-compatible, no debug mode required" is recorded as a confirmed design goal (NFR-6). If not: the exact consequence is recorded and requirements are revisited.
5. The number of bytes each test export line adds to `debug.log` is recorded (feeds the log-budget work in M3/M6a).
6. The stub mod and protocol are the only artefacts; nothing from M0 is shipped.

**Status (2026-10-04): run by the owner; result PASS (T6 resolved by M0b below).** T0, T1 (normal launch and Ironman) and T2 (normal launch and Ironman) pass; `debug_log` needs no `-debug_mode`; a debug-mode launch showed achievements "Not available". T6 is partial (names and numbers resolve with some scope forms; others don't). Record: `docs/spikes/m0-results.md`. Per the protocol, a partial T6 needs the owner's decision before M1: proceed and pin the remaining forms in the first M3 iteration (recommended), or run a short M0b first.

**M0b (2026-10-04, run by the owner): T6 resolved.** One more launch (King Harold of England, 1066) showed that `THIS`-based forms resolve for the ruler and from heir, vassals, wars and claims, that a vassal's or heir's opinion of the ruler is available via a `"opinion(liege)"` script value or a precomputed variable (saved scopes inside script values do not work), and that an 8,000-character line survives intact. Not covered: faction-scope forms (no faction at the test ruler), left for M3. Record: `docs/spikes/m0-results.md`. **All M0 acceptance criteria are met; per criterion 3, M1 starts only when the owner confirms.** Cleanup was partial (stub removed, debug mode off, usual playset restored; throwaway saves kept; the usual campaign's achievements were already disabled before M0).

## M1. Skeleton + CI + repository standards
Empty SDL3 + ImGui window building on Windows, macOS and Linux through GitHub Actions, in a repository set up as a tidy open-source project (D14, section 9 of `requirements.md`).

### M1a. Build skeleton
CMake (≥3.25) + `CMakePresets.json` + vcpkg manifest (`builtin-baseline`; `sdl3`, `imgui` with `sdl3-binding` and `sdl3-renderer-binding`, `curl`, `nlohmann-json`, `doctest`), release-only overlay triplets (static; `x64-windows-static`, `arm64-osx`, `x64-osx` overlay with `VCPKG_OSX_DEPLOYMENT_TARGET=15.0` (CK3's macOS floor), `x64-linux`; Linux floor Ubuntu 24.04 = glibc 2.39), `app/` with `SDL_Renderer` + ImGui window and the `IRenderBackend` seam, event-driven loop (`SDL_WaitEvent`), `platform/` stubs, one `ci.yml`.

### M1b. Repository standards (RS-*)
Files (committed): `README.md` (install/usage per OS, troubleshooting, the one-line credit "Inspired by Voices of the Court.", checksum/attestation verify steps once releases exist), `LICENSE` (MIT), `CONTRIBUTING.md` (build/test, commit convention, PR flow, licensing of contributions), `CODE_OF_CONDUCT.md` (Contributor Covenant 3.0 with attribution and a real contact), `SECURITY.md`, `SUPPORT.md`, `CHANGELOG.md` (Keep a Changelog 1.1.0 with `[Unreleased]`), `.github/CODEOWNERS`, `.github/ISSUE_TEMPLATE/{bug_report,feature_request,tester_report}.yml` + `config.yml` (blank issues off; links to private vulnerability reporting and docs), `.github/pull_request_template.md`, `.github/dependabot.yml` (github-actions + vcpkg), `.github/release.yml` (generated-notes categories), `.editorconfig`, `.clang-format`, `.clang-tidy`, `.gitattributes` (LF), `.gitignore` (**includes `reference/`; created before the first commit**), `THIRD_PARTY_NOTICES` generation script, SPDX one-liners in source files.

[MANUAL-GH] checklist (owner, repo admin; written into `CONTRIBUTING.md`'s maintainer section and ticked here):
1. Ruleset on the default branch (Active): require pull request with **0 required approvals** (a solo author cannot approve their own PR), require the status checks **`CI result`** and **`PR title`** (create the ruleset only after the first CI run, since a check can be selected only once it has run), require linear history, block force pushes and deletions, resolve conversations; admin bypass kept for emergencies; allowed merge method **squash only**.
2. Settings → Actions → General: default `GITHUB_TOKEN` read-only; require actions pinned to a full commit SHA.
3. Settings → Advanced Security: private vulnerability reporting, dependency graph, Dependabot alerts + security updates, secret scanning + push protection; enable CodeQL for C/C++ (scheduled, not a PR gate; see M7).
4. Labels created (small set: bug, enhancement, documentation, needs-triage, tester-report, platform:*, area:*, good first issue, help wanted, breaking-change, skip-release, dependencies, ci, security).
5. 2FA on the maintainer account. Release immutability is enabled in M7 before the first public release.
6. Verify Insights → Community Standards shows every item green.

### Acceptance criteria
1. CI is green on windows-2022, macos-26 (arm64) and ubuntu-24.04 from a clean checkout (images are pinned: `windows-latest` is now Visual Studio 2026 only and `ubuntu-latest` moves to 26.04 in November 2026); the second run uses the vcpkg binary cache (`actions/cache` over the `files` provider, SHA-pinned).
2. macOS job also builds x86_64 (cross, overlay triplet) and produces a universal binary via `lipo` (artefact, unsigned). The x86_64 slice is build-verified in CI and smoke-run under Rosetta 2 on the owner's Mac **[OWNER-MAC]** (Rosetta is not real Intel hardware: Intel behaviour remains [TESTER]).
3. The app opens an ImGui window, renders a frame, and exits cleanly; the `doctest` target runs under `ctest` in CI with at least one passing test.
4. Idle check: with the window hidden/minimised, the process is blocked in `SDL_WaitEvent` with zero wake-ups over 10 s (measured on macOS with `sample`/`top`; on all CI OSes via a frame-counter assertion in a debug build).
5. A libcurl HTTPS request succeeds on all three CI OSes **or** the CA-bundle strategy is chosen and documented (A16: libcurl's own CA handling works on macOS and Linux, see research-notes §2.15; the probed bundle is only a fallback).
6. CI job structure: build/test matrix, `lint` (clang-format check with a pinned version; scan, version and mod lint), `sanitizers and clang-tidy` (Linux, ASan+UBSan on the unit tests, core only), and an aggregator job named **`CI result`** (`if: always()`, fails unless every needed job succeeded). The Conventional Commits PR-title lint is a separate workflow and required check **`PR title`** (it must re-run on title edits). No `paths:` filters on required workflows. Third-party actions pinned to SHAs; workflow `permissions:` read-only by default.
7. Repository-standards files above exist and pass a lint (YAML valid; issue forms accepted by GitHub); [MANUAL-GH] checklist ticked; **spikes:** a scratch PR proves the ruleset admin bypass and the `CI result` aggregator behave as intended (a deliberately failing matrix leg turns `CI result` red).
8. No file under `reference/` or from Paradox is tracked (CI check); a CI step greps for engine-line patterns and API-key-shaped strings (test fixtures use obviously fake keys).
9. Dependabot is configured for github-actions; its first vcpkg PR (#1) proposed an older release tag, so Dependabot is told to ignore the vcpkg baseline and the baseline is bumped by hand (research-notes C21, §4.2).

Tests: [CI], [LOCAL], [MANUAL-GH]. Owner: run the build on the Mac (smoke).

**Status (2026-10-04): implemented; CI is green on GitHub (run 37215391227, third attempt) on Windows, macOS arm64, macOS x86_64 + universal binary and Linux x64.** The first run failed on two setup mistakes (PyYAML not pinned for the lint job; Linux system packages), the second on one more (`libltdl-dev`); all were CI-recipe fixes, not code defects. Cold builds took 90–270 s per OS, warm macOS builds about 40 s (vcpkg binary cache works). The `macOS universal binary` job merged both slices with `lipo` and ran them (arm64, x86_64 under the runner's Rosetta, `--smoke`).
- Verified on the owner's Mac (arm64, macOS 26.5.1): clean build with `-Werror`; 9 unit tests, headless smoke test, idle test and HTTPS test pass; the idle loop drew no frames and used the same CPU time for a 2 s and a 20 s idle run (0.14 s in both, i.e. about zero while idle; peak RSS about 88 MB); the x86_64 slice cross-builds, merges with `lipo` into a 7.4 MB universal binary and passes `--version` and `--smoke` under Rosetta (criterion 2).
- Verified in an Ubuntu 24.04 container (arm64 Linux, so an arm64 copy of the Linux triplet was used): the exact CI recipe (shallow vcpkg fetch of the pinned baseline, trimmed apt list, SDL3 from vcpkg, GCC 13 `-O2 -Werror`) builds, and all 11 tests pass under `xvfb-run`, including the idle test and HTTPS with default CA handling. All sources also compile cleanly with GCC 13 `-Werror`.
- **Verified by CI after the first pushes:** Windows (VS 2022 generator, static CRT, `-Werror` on MSVC, unit/smoke/idle/HTTPS tests), the x86_64 Linux triplet, the cache, artifacts and the `CI result` aggregate, clang-tidy, and Dependabot's config. The `PR title` workflow, the dependency-review job and the branch ruleset were then exercised on pull requests (#2, #3: a bad title failed the check, valid ones passed, merges went through the ruleset). **Still unverified:** issue-form acceptance by GitHub.
- Deferred, recorded: the event loop draws only after events, so time-driven ImGui features (text-caret blink, held-button repeat, tooltip delays) will need `SDL_WaitEventTimeout` while such an item is active (M4); the `IRenderBackend` seam will take options and own window flags when the transparent overlay needs them (M2); Windows user-folder lookup should use `SHGetKnownFolderPath` (M3).

## M2. Overlay spike
Transparent always-on-top window over CK3 on Windows and macOS. macOS fullscreen Spaces is the biggest risk (C1). Target mode: CK3's default **Fullscreen** (on the Mac a native macOS Space, owner-observed 2026-10-05; the "borderless underneath" reports are not Mac-specific, C19/C26), fallback **Windowed** (D13). Because no macOS overlay over CK3 is confirmed anywhere (VOTC-CE: C25; the separate VOTC org repo: claimed in commit messages only, C33), M2 follows an explicit fallback ladder (FR-OVL-8): (1) overlay over Fullscreen; if that can't appear, (2) overlay with CK3 Windowed; if that fails, (3) a normal companion window (e.g. on a second monitor). The result records which rung works.

**First step [OWNER-MAC]: done (2026-10-05).** Graphics offers **Fullscreen** and **Windowed** (set to Fullscreen); Fullscreen is a **native macOS Space** (CK3 appears as its own desktop in Mission Control). The primary M2 target is therefore the native-Space case, with Windowed as the first fallback.

**Scope (owner decision 2026-10-05):** one time-boxed macOS session covering criteria 1–5 and 8, plus a stub scripted-widget probe for T7 in the same session. Windows and Linux (criteria 6–7) stay CI plus tester checklist.

**Confirmed by the owner 2026-10-05 (in `docs/spikes/m2-checklist.md`):** the experiment order and stop rules, with E3 automatic (a timer cycles the level and flag combinations), the probe built as a `.app` bundle and launched with `open`, the hotkey test limited to Cmd- or Ctrl-based bindings, and no VOTC black-box check. E3 (does the panel recipe appear over the native Space at all) decides the Fullscreen experiments E4–E7; if it fails at every level and flag combination tried, record that, skip E4–E7, and go on to the Windowed rung. The hotkey, game-detection and tray experiments (E8, E9, E11) do not depend on the overlay being visible and run regardless. The probe (`app/probe/`, a macOS app bundle) and the T7 stub mod (`mod/curia-m2-probe/`) exist and were exercised without CK3 only; the checklist has their build and launch commands.

Work: macOS Objective-C++ in `platform/mac` (Accessory policy, non-activating `NSPanel`, `screenSaver` level, collection-behaviour combinations from the spike list; Apple DTS thread 826308 re-read 2026-10-05; the native recipe and the SDL 3.4.18 facts are in research-notes §2.5; wrapped-`NSPanel` and key-input behaviour are the main unknowns), transparent `SDL_Renderer`+ImGui window, focus take/return, Carbon hotkey, tray; Windows equivalents (topmost, `WS_EX_NOACTIVATE` toggling, `SetForegroundWindow` return, `RegisterHotKey`); game-window finder. A plain always-on-top chat window is **not** a fallback over CK3's macOS Fullscreen (M2: not visible, E1/E2); the expected fallback there is a normal companion window (untested; rung 3 was not exercised). An attached overlay is the goal where it works (the same fallback VOTC-CE added for Linux/Proton).

**Status (2026-10-05): done for CK3 Fullscreen; Windowed is deferred to the M4 testing (owner decision, not a failure).** The overlay panel (non-activating `NSPanel` under the Accessory policy) was visible over CK3's native Fullscreen Space at all 20 level and flag combinations, and the plain SDL window was not (owner observations). Results, evidence and the design consequences: `docs/spikes/m2-results.md`. Open: typing with CK3 frontmost (owner-observed, not confirmed in the log; re-check in M4), click-through that holds (low priority), Windowed, rung 3.

Acceptance criteria:
1. **macOS [OWNER-MAC]:** for each CK3 display mode the Mac offers, a table records whether the overlay is visible above CK3, is clickable, takes typed input, and where focus lands on close. The supported mode(s) are documented; modes that cannot work are listed with the reason. **The fallback ladder is walked in order and the working rung is recorded:** (1) overlay over Fullscreen, (2) overlay with CK3 Windowed, (3) normal companion window. **Result:** Fullscreen **pass** (visible and clickable; typed input reaches the overlay, with CK3 frontmost owner-observed and not confirmed in the log; Esc returns keys to CK3 with no click); rung 1 of the ladder works; **Windowed deferred to the M4 testing**.
2. macOS: no Accessibility/Input Monitoring prompt appears for the Carbon hotkey on macOS 26.5.1 (or the prompt is documented and the hotkey is demoted to optional). Default binding uses Cmd or Ctrl (C10). **Result: pass** (the Carbon hot key fired with CK3 frontmost, no prompt).
3. macOS: opening the overlay does not pull CK3 out of the foreground in a way that pauses or minimises it; closing returns keyboard input to CK3 with no click needed (or the exceptions are documented). **Result:** open and close **pass**; typing with CK3 frontmost is owner-observed (no reaction in the game), not confirmed in the log; re-check in M4.
4. macOS: per-pixel transparency renders correctly (no dark fringe); idle CPU stays ~0 with the overlay open and static. **Result: pass** (no fringe; static CPU about 0.5 ms per second, zero redraws).
5. Tray: menu-bar item with Show/Hide/Quit works and the Dock icon behaves as intended. **Result: partial.** The tray works, but the menu bar did not appear on CK3's Fullscreen display (two-display setup), so the tray is secondary (in-game button and hot key are primary, owner decision); creating the tray activates the app; no Dock icon (the owner's earlier "Dock icon: yes" meant the menu-bar icon).
6. **Windows [CI]:** builds, window-creation smoke test passes. **[TESTER]:** a checklist (`docs/spikes/tester-checklist-windows.md`, mirrors the tester-report form) is handed to a tester: overlay over Fullscreen vs Windowed, single and hybrid GPU transparency, focus return, hotkey. Not blocking; recorded as unverified. **Result:** CI green for the probe branch (run 37308965499 at `7353aaf`); tester items not run (unverified).
7. Linux X11/XWayland: builds in CI; `SDL_VIDEO_DRIVER=x11` window-creation smoke under Xvfb; behavioural checks are [TESTER]. **Result:** CI green for the probe branch (same run); tester items not run (unverified).
8. **macOS [OWNER-MAC]:** the overlay can find CK3's window rectangle without any permission prompt (or the prompt is documented and a manual-position fallback exists). **Result: pass** (rectangle found without a prompt; CK3's window is not identical to the display, it starts 39 points lower).
9. `research-notes.md` M2 spike list is updated with the answers. **Result: done** (`research-notes.md` §2.5, §5.2, C34; `m2-results.md`).

## M3. Mod spike
In-game button writes a marker to `debug.log`; the app detects it. Answers: log flush latency, clipboard vs log trigger, line format, log budget. (T0, T1, T2 and T6 were answered in M0 and are not repeated.)

All [OWNER-MAC] in game. The real mod skeleton (`mod/curia/`), a test protocol sheet and the log-reading tool are prepared first; the owner plays and reports. Any step using `-debug_mode` + console runs in a **throwaway game only**.

**Status (2026-10-05): preparation done; the owner sessions have not run.** Prepared and checked without the game: the real spike mod `mod/curia/` (two HUD buttons, the `CURIA1` export frame, a probe section for unproven forms; ck3-tiger clean), `tools/m3/install_mod.sh` (installer with the T11 variants), `tools/m3/logwatch.py` (millisecond-polling log watcher with unit tests that run in CI) and the owner's session sheet `docs/spikes/m3-protocol.md`, split into two sessions: Session 1 (T10, T5, T4, T3, T7 repeat, error.log and achievements checks) is what the wire-format freeze needs; Session 2 (T8 hour and baseline stretches, T11 launcher variants) can be a different day. After the results: the wire-format freeze, the watcher and parser in the app, the fuzz harness, `tools/fake_ck3` and the fixtures (acceptance criteria 4 and 5).

Test order:
1. **T10** `script_docs` / `dump_data_types` behaviour on the Mac (throwaway game); populate local `reference/ck3/`.
2. **T3–T5** flush latency, line format/length/charset, truncation at launch. (The owner's `debug.log` already shows the engine line shape: bracketed time, level, source file and line, then text; multi-line entries exist.)
3. **T7** scripted-widget button: visible, clickable, root scope = player, behaviour in each display mode and on pause. (First pass done in the M2 session with a stub: **Fullscreen pass**, clicks recorded unpaused and paused with name, title, gold and date; the Windowed check is deferred to the M4 testing; M3 repeats it with the real mod and decides.)
4. **T8** bytes per export; total bytes appended per hour of normal play; cap reproduction (long run).
5. **T9** optional clipboard-vs-log comparison.
6. **T11/T12** metadata/launcher behaviour; local ck3-tiger pass (`--game <install path>`).

Work: real mod skeleton (widget, scripted GUI, minimal export), `tools/fake_ck3` v0 with the real format after capture, watcher + frame parser in the app with unit tests (fuzz harness on the pure parser function), `docs/wire-format.md`, `tests/fixtures/` from real captures (Curia marker lines only).

Acceptance criteria:
1. Every T-question in this milestone has a recorded answer in `research-notes.md` with the date, game version, and OS; none is estimated.
2. Pressing the in-game button produces a marker the app detects; measured latency distribution (not a guess) is recorded for pause, menu and fast-forward.
3. Trigger decision (D2) is confirmed or revised with the data; if clipboard (T9) wins, the owner decides.
4. The wire format is frozen as `CURIA1` in `docs/wire-format.md` (macOS-verified only; Windows/Linux log behaviour goes to the tester checklist), with bytes-per-export and bytes-per-hour numbers and a log-budget threshold default derived from T8.
5. Parser unit tests pass against real captured fixtures; the libFuzzer harness runs a fixed budget in CI; fake CK3 reproduces the captured structure (prefix shape, interleaving, truncation) and the app handles it in a CI end-to-end test.
6. Mod loads with no `curia_` entries in `error.log`; ck3-tiger local run reviewed; supported-version behaviour (T11) documented.

## M4. Vertical slice
Button → export gold + heir → overlay opens → question to the Anthropic API → streamed answer.

Work: gold/income + heir/succession export; `AnthropicMessages` provider (streaming, caching blocks, errors, cancel); `PromptBuilder`; chat UI with streaming; dev-only key from env var `CURIA_ANTHROPIC_API_KEY` and dev-only model from env var `CURIA_MODEL` (never shipped, never logged; proper storage and the models endpoint are M5), so no model literal enters source.

Acceptance criteria:
1. **[OWNER-MAC]** In CK3, click Advisor → overlay opens within the latency measured in M3 → ask "is my heir safe?" → the answer streams token by token and references real values from the snapshot (gold, heir).
2. **[CI]** The same flow passes end-to-end on all three OSes with fake CK3 + mock Anthropic SSE (including mid-stream `event: error`, `ping`, split chunks).
3. Cancel stops generation promptly; the connection is closed (A11 verified or documented).
4. Cache usage fields are logged; with persona + snapshot above the model's minimum, a second question in the same snapshot shows `cache_read_input_tokens > 0` (or the reason it can't is recorded).
5. Log-silence detection shows the "restart CK3" message when the fake CK3 dies after N bytes.
6. No API key in logs or crash output (test greps the app log; CI key-pattern scan clean).
7. **[OWNER-MAC]** Deferred from M2: with CK3 in **Windowed**, the overlay is visible, clickable, takes typed input and returns the keys to CK3 on close, and the in-game button works; with CK3 **Fullscreen**, typing in the overlay while CK3 is frontmost does not reach the game (the E6 check: owner-observed in M2, not confirmed in the log). The overlay appears on CK3's display (FR-OVL-9), no Dock icon shows (`LSUIElement`), and the tray, if shown, is created at start-up.

## M5. Providers + settings
OpenAI-compatible provider, settings UI, secure key storage per OS, models endpoint (D4). (Agent Mode moved to M8, post-v1.)

Acceptance criteria:
1. OpenAI-compatible provider passes mock-server tests (all reasoning keys, usage chunk variants, comment lines, mid-stream errors, `[DONE]`) **[CI]**, and is verified live against at least one local server and one hosted service the owner uses **[OWNER-MAC]** (candidates: Ollama/LM Studio, OpenRouter).
2. Model dropdown is populated from each provider's models endpoint; on failure free text works; default selection: newest Sonnet-tier for Anthropic (name match), no default for other OpenAI-compatible providers; **no model ID literals in code** (CI grep check) (D4, A13 resolved).
3. Key storage: macOS Keychain **[OWNER-MAC]** (including behaviour of an ad-hoc-signed app across updates, spike); Windows Credential Manager and Linux libsecret implemented, tested in CI where possible (libsecret in a CI container with a keyring provider), live checks [TESTER]. With no Secret Service provider on Linux, the key is session-only in memory (D11) and the UI says so. Keys never written to the settings file.
4. Settings UI covers FR-SET-1; settings persist across restarts.
5. Redaction tests: keys never appear in the app log, crash output or exported diagnostics.
6. CA-bundle strategy (A16) verified against `api.openai.com` and `api.anthropic.com` on a clean machine/user for macOS **[OWNER-MAC]**; Windows/Linux [TESTER].

## M6a. Full advisor (v1)
Full realm export, prompt caching tuned, advisor quality.

Acceptance criteria:
1. Export includes gold/income, heir and succession, top vassals with opinions, factions, wars, claims; per-export size is within the budget from M3 T8; sections can be switched off.
2. Prompt caching: persona and snapshot are separate breakpoints; measured cache-hit rate across a realistic 30-minute session is recorded; TTL choice (5 min vs 1 h) justified with data.
3. **[OWNER-MAC]** The owner asks ten representative questions ("what should I focus on?", "is my heir safe?", "who might rebel?") and rates the answers; failures lead to prompt changes, not code changes. No fabricated data (answers cite only fields present in the snapshot).
4. Snapshot size vs context cost documented; Haiku-class minimums (4,096 tokens) noted for users picking small models.
5. A CK3 patch regression checklist exists for the mod (what to re-test on every patch).

## M6b. Council personas (post-v1, confirmed)
Steward, Marshal, Spymaster, Chancellor advise in character; skill affects advice quality (FR-ADV-5). Requires council data in the export and per-persona prompts. Acceptance criteria are written when scheduled.

## M7. Packaging and publishing (v1.0)
Release parts of the repository standards (RS-REL, RS-SEC, RS-LIC).

Work and acceptance criteria:
1. **Release workflow** (`release.yml`, tag `vMAJOR.MINOR.PATCH`, pre-releases like `v0.5.0-rc.1` marked prerelease): `verify` job checks the tag equals the CMake `project(VERSION)` and `CHANGELOG.md` has a matching section; build matrix (Windows zip, macOS dmg, Linux AppImage, mod zip) with versioned filenames; one `SHA256SUMS`; build-provenance attestations with `actions/attest` (SHA-pinned; `id-token: write`, `attestations: write` only on that job); the release is created as a **draft**, all assets uploaded, then published (required once **immutable releases** are enabled: [MANUAL-GH] before the first public release). Dry-run first on a scratch tag (`v0.0.0-test.N`), including the draft→publish path and `gh attestation verify` on every artefact.
2. Mod uploaded (private) to Steam Workshop via the launcher; install from the Workshop on the owner's Mac loads and works **[OWNER-MAC]**; metadata (`id` `curia`), thumbnail, description in place; the Paradox Mods path is confirmed or dropped here.
3. Windows: static-CRT zip artefact from CI containing `curia.exe`, LICENSE, `THIRD_PARTY_NOTICES`, README, mod folder; smoke-run by a tester **[TESTER]**; unsigned binaries show SmartScreen warnings: documented ("More info → Run anyway"); free signing options (SignPath Foundation) evaluated.
4. macOS: universal binary in a dmg. **Interim path (if the Apple Developer account is not yet active):** ad-hoc-signed, un-notarized dmg with README steps for current macOS: open once (it is blocked), then System Settings → Privacy & Security → **Open Anyway**; right-click → Open no longer works for un-notarized apps from macOS 15; versioned filenames, SHA-256 beside the download, a "why un-notarized" note (VOTC-CE ships this way, with unversioned filenames and no checksums). **Final path:** Developer ID signed, hardened runtime, **notarized** and stapled; Gatekeeper accepts it on a clean user account **[OWNER-MAC]**; the x86_64 slice runs on a real Intel Mac **[TESTER]** (Rosetta smoke on the owner's Mac otherwise). The Apple Developer account is arranged just before this milestone (D10, R7).
5. Linux: AppImage built on the `ubuntu-24.04` runner (the floor is CK3's own, Ubuntu 24.04 / glibc 2.39; an older container is needed only if the floor is lowered); also a plain `tar.gz`; runs under X11/XWayland with `SDL_VIDEO_DRIVER=x11` **[TESTER]**; reads a Proton prefix log.
6. `THIRD_PARTY_NOTICES` generated from vcpkg `copyright` files plus hand entries (fonts, bundled CA bundle if used); SPDX identifiers present; no Paradox content or VOTC code in any artefact (final audit).
7. README: install steps per OS, how to enable the mod, how to set an API key, display-mode guidance (Fullscreen/Windowed), troubleshooting (log silence → restart CK3; never use the console), checksum and `gh attestation verify` steps, the one-line VOTC credit, license, third-party licenses.
8. Security hygiene verified: secret scanning and push protection on; `SECURITY.md` current; release secrets (signing, notarization) only in a protected `release` GitHub Environment; CI tests use fake keys and a local mock server; CodeQL (C/C++, scheduled) and OpenSSF Scorecard (weekly) running; Best Practices badge deferred until after 1.0.
9. At least one Windows and one Linux tester have run the release candidate; their reports are in the repo. Anything unverified is listed under "Known limitations". `CHANGELOG.md` has the 1.0.0 section; version is `1.0.0` only if the public contract (wire protocol, config schema) is declared stable, otherwise stay `0.y.z`.

## M8. Agent Mode (post-v1)
Claude Code CLI as a provider, **API-key auth only**, scoped **read-only** tools (D3). Bedrock / Google Cloud Agent Platform / Foundry stay out until someone can test them. Kept here so the research is not lost; **re-plan before starting** (the CLI evolves quickly).

Design notes (from the 2026-10-04 research, `research-notes.md` §2.10): confinement does not come from allow/deny path rules (deny beats allow; no "allow one folder, deny the rest"). It comes from `cwd` = a Curia-owned data directory with no `--add-dir`, `--tools "Read,Grep,Glob"` (Glob and Grep must be named explicitly on macOS/Linux; omitting Bash removes it), `--permission-mode dontAsk` + `--permission-prompts none`, `permissions.blockReadsOutsideWorkingDirectories: true` via `--settings`, plus targeted deny rules. `--bare` does **not** restrict tools; the newer `--restricted` flag (v2.1.248+) looks like a better fit and must be spiked with API-key auth. The Bash sandbox does not cover the Read tool.

Acceptance criteria (draft):
1. Spikes S1–S10 (`research-notes.md` §5.3) answered, including: exactly Read/Grep/Glob listed in `system/init`; reads outside the data dir (including via symlinks and `../`) denied; hostile `~/.claude` and `.claude/` folders have no effect; `--bare` vs `--restricted` decision; cost/latency of the default Claude Code prompt vs the plain Anthropic provider (Agent Mode is slower and costlier; it is justified only when the model navigates a growing history/notes folder).
2. Environment builder unit-tested: only `ANTHROPIC_API_KEY` plus the minimal environment reaches the child; `ANTHROPIC_AUTH_TOKEN`, `CLAUDE_CODE_OAUTH_TOKEN`, `CLAUDE_CODE_USE_*` never forwarded; `CLAUDE_CONFIG_DIR` Curia's; prompt on stdin; argv always contains the isolation flags; the app fails closed if `system/init` lists Bash or any unexpected tool.
3. UI label "Agent Mode (powered by Claude)"; no "Claude Code" branding anywhere (string audit); minimum `claude` version enforced.
4. Live verification on the owner's Mac **[OWNER-MAC]**; Windows (`claude.exe`, process-tree kill) **[TESTER]**.
