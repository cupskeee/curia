# Curia requirements

Status: draft v2, 2026-10-04 (owner decisions of 2026-10-04 applied). Source of truth for *what* Curia must do. Evidence and links: `research-notes.md` (cited as C#, D#, A#, T# below). Design: `architecture.md`. Plan: `milestones.md`.

## 1. Purpose

Curia is an in-game AI strategy advisor for Crusader Kings III. The owner is a new CK3 player who finds it overwhelming to manage everything at once. The player opens a chat window over the game and asks the advisor about their realm ("what should I focus on?", "is my heir safe?"). The advisor sees live game data exported by a mod.

Inspired by the mod "Voices of the Court" (VOTC). Curia is **not** a fork and contains no VOTC code or script (VOTC's app is GPL-3.0, VOTC's mod is CC BY-SA 4.0; C6). The README credits it in one line ("Inspired by Voices of the Court."). Curia itself is MIT-licensed (D5), app and mod alike.

## 2. Scope

**v1 (read-only advisor):** in-game Advisor button, realm export via `debug_log`, overlay chat, **two** LLM backends (OpenAI-compatible and native Anthropic Messages API), settings with secure key storage, packaging for Windows/macOS/Linux, and a tidy open-source repository (section 9).

**Post-v1:** Agent Mode (Claude Code CLI, section 4.5), Amazon Bedrock / Google Cloud Agent Platform / Microsoft Foundry credentials for it, council personas (M6b, FR-ADV-5).

**Out of scope:** sending actions back into the game (VOTC-style run files / console commands) in v1; Steam Deck Gaming Mode; multiplayer-specific behaviour.

## 3. Decisions in force

| ID | Decision |
|---|---|
| D1 | Renderer: `SDL_Renderer` + `imgui_impl_sdlrenderer3` behind a thin abstraction (SDL_GPU cannot do transparent windows; C2). |
| D2 | Trigger: log-marker line in `debug.log` is primary. Clipboard marker is an optional comparison in the M3 spike. |
| D3 | Agent Mode is **post-v1**. When it ships: API-key authentication only (enforced by design: `--bare`, explicit child environment, Curia-owned config directory), scoped **read-only** tools (zero tools would only make it a slower, costlier copy of the Anthropic provider). Bedrock / Google Cloud Agent Platform (formerly Vertex AI) / Microsoft Foundry are also post-v1 because the owner cannot test them. |
| D4 | No hard-coded model lists. Fetch from each provider's models endpoint; free-text fallback; default to a current Sonnet-tier model where the provider has one (FR-LLM-4). |
| D5 | License: **MIT**, same for app and mod. |
| D6 | ck3-tiger runs locally only (cargo build on macOS). No CI job touches Paradox content, not even in a private repo with game files: that would put Paradox content in a repository. |
| D7 | Achievement/`-debug_mode` questions are answered first, in **Milestone 0** (stub mod, no app): does `debug_log` need `-debug_mode`, and does that block achievements? Achievement compatibility is a design goal if mods don't block them (C4). |
| D8 | Linux specifics live in `research-notes.md`; this file has a short Linux section. |
| D9 | **Milestone 0 gates all C++.** M0 = T0, T1, T2, T6 with a stub mod; the owner reads `debug.log` by hand. If any test fails, stop and re-plan with the owner before writing C++. |
| D10 | Identifiers: GitHub user `cupskeee`, repo `github.com/cupskeee/curia`, mod id `curia`, macOS bundle id `io.github.cupskeee.curia`. Apple Developer account deferred to just before M7. |
| D11 | Linux key storage with no Secret Service provider: session-only, in memory. |
| D12 | Dear ImGui: accept vcpkg's version (1.92.9); vendor 1.92.9b only if a specific bug requires it. |
| D13 | Target CK3's default **Fullscreen** mode with **Windowed** as fallback. On the Mac the owner reported (2026-10-05) that Graphics offers exactly these two and that Fullscreen is a **native macOS Space**, so the primary M2 target there is the native-Space case; "borderless underneath" is a 2020 Windows player report with later agreeing posts and one hedged staff post, nothing Mac-specific (C19, C26). **M2 (2026-10-05):** Fullscreen passes on the Mac; Windowed is deferred to the M4 testing (owner decision, not a failure); a plain always-on-top window is not a fallback over Fullscreen (`docs/spikes/m2-results.md`). |
| D14 | The project is run as a tidy, fully managed open-source project (section 9). |

## 4. Functional requirements

### 4.1 Mod (CK3 script + GUI)
- **FR-MOD-1** Add an "Advisor" button to the in-game UI. Preferred mechanism: a **scripted widget** (own `.gui` window registered under `gui/scripted_widgets/`), so no vanilla GUI file is overridden (research-notes §2.2). All names carry the `curia_` prefix; the mod id is `curia`.
- **FR-MOD-2** The button runs a scripted GUI effect rooted at the player character that writes the realm export with `debug_log`.
- **FR-MOD-3** Every exported line starts with a fixed, versioned marker prefix (e.g. `CURIA1|…`) because `debug.log` also contains engine lines (research-notes §2.1). The exact line format is fixed in M3 (M0 showed loc expressions resolve, with only some scope forms, and that the engine prefixes every call with about 110 bytes in M0 and 119 to 124 bytes with the real mod (M3 T4), including the script file and on_action/effect name, so the app must find the marker anywhere in the line).
- **FR-MOD-4** The export is a framed message (begin/end, snapshot id, line count) so the app can discard truncated snapshots. M3: the snapshot id is a save-stored counter (continues after loading, restarts at 1 in a new game) and a reload is not announced (`docs/spikes/m3-results.md`).
- **FR-MOD-5** Exports are compact: the log may stop after ~17MB of cumulative writes (C3; unconfirmed). Bytes per export are measured in M3 (T8) and recorded in `architecture.md`; bytes per hour of normal play are measured in normal play after M4. Because the engine prefix costs 119 to 124 bytes per `debug_log` call with the real mod (about 110 in M0; M3 T4), pack many fields per line and keep the number of calls small (a single 8,000-character line survived intact in M0b).
- **FR-MOD-6** v1 export content, small first, then expanded in M6a: gold/income, heir and succession, top vassals with opinions, factions, wars, claims.
- **FR-MOD-7** The mod must not write to `error.log` in normal operation (shared log budget; C3); in particular, never set a variable that script never reads (the engine reports it at load, seen in M0). Validate locally with ck3-tiger (D6); expect false positives while ck3-tiger's latest release targets 1.19 and the game is 1.20 (C17).
- **FR-MOD-8** The mod uses no console commands and no run files (read-only; keeps achievement/Ironman compatibility possible).
- **FR-MOD-9** `supported_version` tracks the current patch (1.20.* today); see risk R3.

### 4.2 Companion app: game bridge
- **FR-BRG-1** Locate the CK3 user folder: Windows/macOS `Documents/Paradox Interactive/Crusader Kings III/` (observed on the owner's Mac, research-notes §2.1); Linux native and Proton paths probed (section 7); user override in settings. The log is `logs/debug.log`.
- **FR-BRG-2** Watch `debug.log` event-driven (OS file-change notification, with a low-rate stat fallback; whether notifications fire reliably is A15, not tested in M3 because the watcher polled, and no notification test exists yet). Read only appended bytes; handle truncation at game start (T5: M3 saw `debug.log` and `error.log` truncated in place, same inode, not recreated; `docs/spikes/m3-results.md`).
- **FR-BRG-3** Parse only marker-prefixed lines; ignore all others. Reassemble framed snapshots; ignore incomplete ones; tolerate duplicates and out-of-order ids.
- **FR-BRG-4** A complete new snapshot becomes the current realm state and opens the overlay (button trigger).
- **FR-BRG-5** Detect and report "log went silent": no marker within a timeout after the user presses the button, or the total bytes appended to `debug.log` since the CK3 process started (a lower bound on the cumulative writes the cap counts, since `error.log` shares it) approach a configurable warning threshold. The remedy shown to the user is "restart CK3". Curia never tells the user to use console commands (they disable achievements; C3/C4).
- **FR-BRG-6** Clipboard-marker trigger: not built in v1 unless the M3 comparison (T9) shows a clear advantage.

### 4.3 Companion app: overlay and input
- **FR-OVL-1** Transparent, borderless, always-on-top chat window positioned over the CK3 window. Target mode: CK3's default **Fullscreen** (on the Mac a native macOS Space, owner-observed 2026-10-05; on Windows "borderless underneath" is an unverified player report, C26/A8), fallback **Windowed** (D13). The Mac offers exactly Fullscreen and Windowed (owner-reported).
- **FR-OVL-2** Takes keyboard focus when open; returns focus to CK3 when closed (platform layer: SDL has no API for this).
- **FR-OVL-3** Styled for a medieval game: bundled redistributable font, parchment-like palette. No network fetch for assets.
- **FR-OVL-4** Opening: the **in-game button and the global hotkey are the primary controls** (owner decision after M2, 2026-10-05); the tray menu is secondary. Closing: Esc, close control, hotkey.
- **FR-OVL-5** Chat: streaming tokens, cancel generation, conversation history within a game session, "new snapshot" indicator, copy answer.
- **FR-OVL-6** Tray icon (`SDL_CreateTray`): Show/Hide, Settings, Quit; **secondary**. On macOS the menu bar does not appear on CK3's Fullscreen display, so the tray cannot be relied on while playing (two-display setup, M2); creating it activates the app, so it is created at start-up, never while CK3 is frontmost. Best effort on Linux (C8).
- **FR-OVL-7** Global hotkey (primary, on by default, configurable): Windows `RegisterHotKey`; macOS Carbon `RegisterEventHotKey` (**verified in M2 on macOS 26.5.1: fires with CK3 frontmost, no permission prompt**; the default binding includes Cmd or Ctrl, C10); Linux per research-notes §2.6.
- **FR-OVL-8** Display fallback ladder (M2 records which rung works, per OS): (1) the overlay over CK3's default Fullscreen (**works on the Mac, M2**); if it cannot appear, (2) the overlay with CK3 in Windowed (deferred to the M4 testing); if that fails too, (3) a normal companion window (for example on a second monitor). The app is fully usable as a normal window.
- **FR-OVL-9** Placement: every time the overlay is shown it is placed on the display that contains CK3's window (macOS: the bounds of the `ck3` process's largest window from `CGWindowListCopyWindowInfo`, readable without a permission, M2), not on the display under the pointer; the pointer's display is only the fallback when the window cannot be found.

### 4.4 LLM providers (one `Provider` interface)
- **FR-LLM-1** Common interface: `streamChat(messages, onToken)` plus cancel, usage and typed errors (see `architecture.md`).
- **FR-LLM-2 OpenAI-compatible (v1):** base URL + API key (optional for local) + model. Covers OpenAI, OpenRouter, DeepSeek, Ollama, LM Studio, llama.cpp server. One tolerant SSE parser: `[DONE]`, comment lines, empty `choices`, mid-stream `error` objects, reasoning keys `reasoning_content` / `reasoning` / `reasoning_details` (C14).
- **FR-LLM-3 Anthropic native Messages API (v1)** (not the OpenAI-compat layer, which lacks caching and thinking output). Prompt caching on the stable system prompt and on the realm snapshot as separate breakpoints; optional 1-hour TTL. Minimal request parameters: no `budget_tokens`, no sampling overrides, no forced tool choice (C13). Thinking text only if the user enables it (`display: "summarized"`).
- **FR-LLM-4 Models (D4):** populate the model dropdown from the provider's models endpoint (assumption A13); on failure or for unknown servers accept free text. Defaults: for Anthropic the newest Sonnet-tier model offered, derived by name match on the returned list; for other OpenAI-compatible providers there is no default (the user picks from the list or types one). **No model ID literals in source** (CI grep check); examples appear only in documentation. Post-v1 Agent Mode may add one whitelisted alias constant (`sonnet`, resolved by the CLI).
- **FR-LLM-5** HTTP via libcurl multi interface on a dedicated network thread; stall detection by low-speed limit, not total timeout; cancel by removing the handle.
- **FR-LLM-6** Errors are typed (auth, rate limit, overloaded, network, cancelled, bad request) and shown in plain language; partial text is kept.

### 4.5 Agent Mode (powered by Claude): **post-v1**
Kept in the docs so the design is not lost. Nothing here is built or scheduled for v1; scope is re-planned when it is picked up (milestone M8 in `milestones.md`).
- **FR-AGT-1** Detect installation with `claude --version`. The UI shows only the parsed numeric version and generic text such as "the `claude` command-line tool" (the raw output contains "Claude Code" branding, FR-AGT-7); install help links to Anthropic's setup page.
- **FR-AGT-2** Spawn `claude -p --bare --output-format stream-json --verbose --include-partial-messages` with the advisor persona via `--append-system-prompt`; continuity via `--resume <session_id>` (session id read from `system/init`). The prompt and snapshot travel on **stdin**, never as a command-line argument. A unit test asserts `--bare` is always in argv.
- **FR-AGT-3 Auth enforcement (D3).** API key only. Always pass `--bare`, which never reads OAuth or the keychain. Build the child environment from scratch (explicit block, not inherited): only what the child needs (`PATH`, `HOME`/profile, temp dirs, locale, proxy variables if configured) plus `ANTHROPIC_API_KEY`. `ANTHROPIC_AUTH_TOKEN`, `CLAUDE_CODE_OAUTH_TOKEN`, `CLAUDE_CODE_USE_*` and any inherited `ANTHROPIC_*` are never forwarded. `CLAUDE_CONFIG_DIR` is **always** set to a Curia-owned directory so the user's `~/.claude` settings cannot leak in. Secrets never appear on the command line. If no API key is configured, the mode is disabled with an explanation. Subscription login is not selectable anywhere in the UI. (Cloud-provider credentials, post-v1 as well: per-set variable allowlist, no prefix wildcards.)
- **FR-AGT-4 Least privilege: scoped read-only tools.** `--permission-mode dontAsk`, `--permission-prompts none` (claude ≥ 2.1.259; check version), `--max-turns` low, **no Bash, no write/edit, no network tools**. Read-only tools (Read/Grep/Glob) confined to a Curia-owned data directory (for example snapshot history and notes) so the model cannot read other user files. Research (`research-notes.md` §2.10, C27) shows the confinement does **not** come from allow/deny path rules (deny beats allow): it comes from the working directory being that data directory (no `--add-dir`), `--tools` listing only those three tools, `dontAsk` + `--permission-prompts none`, and `permissions.blockReadsOutsideWorkingDirectories`, with targeted deny rules as extras. Verified by spikes S1–S10 before shipping. Rationale: with zero tools Agent Mode is only a slower, costlier copy of the Anthropic provider (owner decision).
- **FR-AGT-5** The working directory is the Curia-owned data directory (FR-AGT-4). It holds only non-secret content, never keys or config, and never a `.claude/` folder or `.mcp.json`, so project hooks/MCP can't load; verified with a polluted-config test.
- **FR-AGT-6** Handle `result` subtypes and `permission_denied` events; SIGTERM/terminate on cancel; clean up the child on app exit (job object on Windows).
- **FR-AGT-7** Branding: the UI label is "Agent Mode (powered by Claude)", matching the allowed forms "Claude Agent", "Claude", "{YourAgentName} Powered by Claude"; never "Claude Code" naming or visuals (Agent SDK branding rule, research-notes §2.8). UI-string audit before release.
- **FR-AGT-8** Whether `--bare` still reads `~/.claude/settings.json` is unverified, so the isolation in FR-AGT-3 is unconditional and a polluted-config test verifies it (with `--setting-sources ''` evaluated as an extra layer).
- **FR-AGT-9** Observe the child's own network and telemetry traffic under `--bare` with the minimal environment (spike) and set any non-essential-traffic opt-out variables the docs provide.

### 4.6 Settings and secrets
- **FR-SET-1** Settings UI: provider, base URL, model, key, cache TTL, thinking display, overlay position/scale, hotkey, log folder override, log-silence thresholds.
- **FR-SET-2** API keys in the OS secure store: Windows Credential Manager; macOS Keychain; Linux libsecret. If no Secret Service provider exists on Linux: **session-only in memory** (D11), with a clear message; never plaintext in the config file. (A development-only environment variable is allowed in M4 and never shipped or logged.)
- **FR-SET-3** Non-secret settings in a JSON file in the per-user config directory.

### 4.7 Advisor behaviour
- **FR-ADV-1** System prompt = advisor persona + rules (concise, actionable, states uncertainty, never invents data not in the snapshot, notes snapshot age).
- **FR-ADV-2** Every question is answered against the latest snapshot; the app tells the model the snapshot's in-game date.
- **FR-ADV-3** Starter questions as buttons ("What should I focus on?", "Is my heir safe?").
- **FR-ADV-4** Sensible length: short by default, expandable.
- **FR-ADV-5** *Post-v1 (M6b, confirmed):* council members (Steward, Marshal, Spymaster, Chancellor) advise in character; skill affects advice quality. Needs council data in the export.

## 5. Non-functional requirements

- **NFR-1 Lightweight.** C++20, CMake, vcpkg manifest mode. Fully event-driven: blocked in `SDL_WaitEvent` when idle; near-zero CPU when the overlay is hidden; no polling loops running per frame. Target: memory footprint small (set a number after M1 measurement; a hard number now would be a guess).
- **NFR-2 Platforms.** Windows and macOS fully supported. **Floors match CK3's own minimum system requirements** (Windows 10 64-bit, macOS 15 Sequoia, Ubuntu 24.04 LTS; `research-notes.md` §2.13). macOS: universal binary (arm64 + x86_64; the x86_64 slice is build-verified and run-verified only under Rosetta 2 or by a tester). Signed + notarized for the final release; an un-notarized dmg is a possible interim path before the Apple Developer account exists (research-notes §2.11). Linux: X11/XWayland supported; native Wayland best effort. Steam Deck Gaming Mode unsupported.
- **NFR-3 Verification reality.** The owner can test **macOS only** (arm64). CI verifies build, unit tests, end-to-end with fake CK3 and window-creation smoke on Windows and Linux; it cannot show that transparency, always-on-top, focus return or log-flush behaviour work. Those are verified by community testers; milestones mark every step that lacks owner verification.
- **NFR-4 Privacy.** Curia's own process talks only to the user-configured LLM endpoint (and its models endpoint). No telemetry from Curia. Realm data goes only to that endpoint. Logs from the app never contain keys. (Post-v1 Agent Mode: the spawned `claude` is a separate program whose own traffic is not established, FR-AGT-9.)
- **NFR-5 Safety of the game.** Curia never writes to CK3 files. It reads `debug.log` only and opens it with shared read access so the game is never blocked (intended; unverified on Windows, where file-sharing semantics matter: tester item).
- **NFR-6 Achievements.** **Confirmed by M0 (2026-10-04):** with the mod and `debug_log`, achievements stay available on a normal launch, also in Ironman, and `debug_log` writes without `-debug_mode`. The design goal is therefore **achievement-compatible and Ironman-compatible operation**: no console commands, no `-debug_mode` requirement, nothing that touches game state. A launch with `-debug_mode` showed achievements as "Not available" from the start in M0 (in the M3 debug launch: "Available" at the setup screen, "Unavailable" after the game started; cause not isolated; `docs/spikes/m3-results.md`), so Curia must never ask users to launch in debug mode.
- **NFR-7 Robustness.** Corrupt/partial log lines never crash the parser (fuzz/fixtures tests). Provider failures never block the UI thread.
- **NFR-8 Resilience to patches.** CK3 major patches arrive months apart (1.19 on 2026-04-20, 1.20 on 2026-09-30) plus frequent hotfixes, and 1.20 had breaking script/GUI changes. The mod avoids vanilla overrides, and we re-validate on every patch.
- **NFR-9 Ground-truth hygiene.** Never commit Paradox-generated content (game script files, `script_docs`, `dump_data_types` output, raw `debug.log` captures). Never copy or closely port VOTC code or script.

## 6. Platform matrix

| | Windows | macOS | Linux (X11/XWayland) | Linux (native Wayland) |
|---|---|---|---|---|
| Support level | full | full | supported | best effort |
| Owner can test | no | **yes** | no | no |
| Overlay | topmost + transparent (D3D11) | Accessory + non-activating panel (M2 risk) | via X11 | not reliable |
| Hotkey | `RegisterHotKey` | Carbon `RegisterEventHotKey` | see research-notes §2.6 | see research-notes §2.6 |
| Key storage | Credential Manager | Keychain | libsecret, else session-only | libsecret, else session-only |
| CI | yes | yes | yes | build only |

## 7. Linux, short version

Supported through X11/XWayland (run with `SDL_VIDEO_DRIVER=x11`; SDL3 defaults to Wayland). Native Wayland is best effort. CK3 under Proton keeps its logs inside the Steam prefix, so the app probes several candidate paths and lets the user override. The in-game button is the primary trigger everywhere. Details, paths and sources: `research-notes.md` §2.6.

## 8. Risks

| ID | Risk | Mitigation |
|---|---|---|
| R1 | macOS overlay can't appear above CK3 in some display modes | M2 spike first; Objective-C++ panel; if some modes can't work, document the supported mode(s) (target: default Fullscreen, fallback Windowed; D13). |
| R2 | `debug_log` needs `-debug_mode`, or values don't resolve in `debug_log` (T6) | Retired by M0/M0b: it works without debug mode, and names, numbers and opinions resolve with `THIS`-based forms (`research-notes.md` §2.14). Faction-scope forms ran in M3 session 2 (three factions, no error); the faction name still carries link markup in the `NoTooltip` form (`research-notes.md` C35, §2.16), and the `special_character` claimant form has not run. |
| R3 | A CK3 patch breaks the mod GUI or script | Scripted widget, `curia_` prefix, local tiger runs, version tags. |
| R4 | Log cap (~17MB cumulative) silences the log mid-session | Small exports, silence detection, restart advice (C3). |
| R5 | Windows/Linux defects the owner can't see, including Windows log-watch behaviour (shared read, buffering, line endings, change-notification timing) which M3 measures on macOS only | CI matrix, fake-CK3 e2e on every OS, tester checklist and tester-report issue form, community beta before M7. |
| R6 | Post-v1 Agent Mode policy drift | API-key-only auth enforced by design; re-check Anthropic docs before M8. |
| R7 | Apple notarization or Developer account delays | Account deferred to just before M7; interim un-notarized dmg path documented. |
| R8 | API keys mishandled (logs, crash dumps, config) | OS secure store, redaction tests, SECURITY.md, private vulnerability reporting (RS-SEC). |

## 9. Repository standards (D14)

The repository is run as a tidy, fully managed open-source project. Everything below is planned into M1 (files, CI, settings) and M7 (release). Evidence and sources: `research-notes.md` §2.11–2.12. Items marked (manual) are GitHub settings that cannot be committed as files; the checklist lives in `milestones.md` M1.

- **RS-FILES** Root / `.github/` files: `README.md` (install and usage per OS, troubleshooting, the one-line credit "Inspired by Voices of the Court.", checksum and attestation verification), `LICENSE` (MIT), `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md` (Contributor Covenant 3.0 with its required attribution; contact: the owner's email, `research-notes.md` §4.1), `SECURITY.md`, `SUPPORT.md`, `CHANGELOG.md` ([Keep a Changelog](https://keepachangelog.com/en/1.1.0/) 1.1.0), `.github/CODEOWNERS`, `THIRD_PARTY_NOTICES`. GitHub's Community Standards page shows every item green.
- **RS-TPL** Issue forms (YAML) for **bug report**, **feature request** and **tester report** (the tester form collects OS and version, CK3 version, mod and app version, CK3 display mode, whether the overlay works, steps, result and an optional log excerpt, and warns not to paste keys), `config.yml` with blank issues off and contact links (private vulnerability reporting, docs), a PR template (summary, linked issue, type, tested-on, no-secrets line, changelog checkbox), a small label set (manual).
- **RS-FMT** `.editorconfig`, `.clang-format`, `.clang-tidy`, `.gitattributes` (LF); formatter and linter versions pinned and enforced in CI.
- **RS-VER** [SemVer 2.0.0](https://semver.org/spec/v2.0.0.html), tags `vMAJOR.MINOR.PATCH`; `0.y.z` until the wire protocol and config schema are declared stable; pre-releases `-rc.N` for tester builds. One repo-wide version for app and mod, stored in CMake `project(VERSION)`; the wire-protocol version (`CURIA1`) is carried separately inside the files (agreed 2026-10-04).
- **RS-COMMIT** [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/) 1.0.0 (`feat`, `fix`, `docs`, `refactor`, `perf`, `test`, `build`, `ci`, `chore`; scopes such as `mod`, `app`, `ui`, `provider`, `export`, `persona`, `release`; `!` or `BREAKING CHANGE:` for breaking changes). Pull requests are **squash-merged**, so the PR title is the commit message; the title is linted in CI.
- **RS-BRANCH** (manual) Ruleset on the default branch: PR required with **0 approvals** (GitHub does not let an author approve their own PR), required status checks **`CI result`** and **`PR title`**, linear history, no force pushes or deletions, conversations resolved, squash-only merges, admin bypass for emergencies only.
- **RS-CI** One CI workflow on `pull_request` and push to the default branch: build/test matrix (Windows, macOS arm64, Linux), lint (clang-format, clang-tidy), sanitizers (ASan+UBSan on unit tests, Linux), fuzz smoke on the pure log-parser function, mod structural lint, secret/engine-line pattern scan, and an aggregator job `CI result`; the Conventional Commits PR-title lint is a separate workflow and required check `PR title`. No `paths:` filters on required workflows. CodeQL (C/C++) and OpenSSF Scorecard run on a schedule, not as PR gates. No job touches Paradox content (D6).
- **RS-SUPPLY** Third-party actions pinned to full commit SHAs (repo policy enforces it); workflow `permissions:` read-only by default and raised per job; no `pull_request_target`; Dependabot for `github-actions` (the vcpkg `builtin-baseline` is pinned and bumped by hand with CI as the gate, because Dependabot only follows release tags and would propose an older baseline; research-notes C21, §4.2); dependency-review on PRs; all third-party code comes through `vcpkg.json`.
- **RS-SEC** `SECURITY.md` (supported versions, private reporting, response time, scope: key storage, key leakage into logs and tester reports, TLS validation, download integrity, the export path); (manual) private vulnerability reporting, secret scanning with push protection, Dependabot alerts and security updates, 2FA on the maintainer account. CI tests use obviously fake keys and a local mock server (never live LLM calls); redaction tests prove keys never reach logs; templates warn users not to paste keys or full logs.
- **RS-REL** Release workflow on `v*.*.*` tags: verify tag = CMake version and a matching `CHANGELOG.md` section; build matrix; versioned artefact names; one `SHA256SUMS`; build-provenance attestations (`actions/attest`) verifiable with `gh attestation verify`; release created as a **draft**, all assets uploaded, then published; (manual) immutable releases enabled before the first public release; signing and notarization secrets only in a protected `release` Environment; dry run on a scratch tag.
- **RS-LIC** `// SPDX-License-Identifier: MIT` one-liners in source, CMake and script files (full REUSE compliance deferred); `THIRD_PARTY_NOTICES` generated from vcpkg `copyright` files plus hand entries (fonts, bundled CA bundle); the mod carries the MIT license and notice too; no Paradox content and no VOTC code anywhere.
- **RS-DOCS** `docs/` is the source of truth; spike results go to `docs/spikes/` and are folded back into `research-notes.md`.

## 10. Corrections to the brief (summary; details in `research-notes.md` §1)

Achievements are *not* blocked by mods since 1.9 (C4); SDL_GPU can't be the renderer (C2); plain `ALWAYS_ON_TOP` is insufficient on macOS (C1); the 17MB cap is anecdotal and can't be bypassed by truncation (C3); clipboard events don't fire in the background on Win/macOS (C5); the VOTC mod repo is CC BY-SA, not GPL (C6); SDL3 defaults to Wayland (C7); `claude -p` doesn't refuse unapproved actions by default (C15, now post-v1); ck3-tiger can't run in CI without game files (C17); M6 vs "personas not v1" resolved as M6a/M6b (C18).
