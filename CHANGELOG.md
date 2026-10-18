# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Until the wire protocol and the config schema are declared stable, versions stay
at `0.y.z` and anything may change between minor versions.

## [Unreleased]

### Added

- The chat window: running `curia` with no mode flag opens a window where you ask your advisor
  questions and read the answers as they arrive. It shows the date of what the advisor can see
  (before the first update from the game, what to do in the middle of the window), notices (the
  last update could not be read, the mod and app versions differ, no update from the game, CK3's
  folder not found; each as tall as its text), a reminder to pause the game (Space) before
  clicking Advisor, a "thinking..." state until the first word and a pulsing dot while the answer
  arrives, **bold** in answers (other markdown is removed; numbered lines stay as written), Stop
  (also Cmd+. on macOS), Retry after an error and Copy on each answer (also on one that was
  stopped or failed part way). Lines are 1.4 em apart with short gaps between paragraphs, and in a
  wide window the text keeps a readable width. It reads CK3's own `debug.log` (or `--log <path>`)
  and shows the realm once you click Advisor in the game. Development builds take the key and the
  model from the environment, as the headless chat does.
- The overlay over CK3 on macOS: the chat opens in a panel that floats over the game, also over
  its Fullscreen mode, without taking CK3 out of it and without a Dock icon. It appears when Curia
  starts and every time you click Advisor in the game, on the display where CK3 is, at the right
  edge of the game. Ctrl+Cmd+K shows and hides it; Esc hides it and is meant to give the keys back
  to the game (what you typed stays, an answer keeps arriving). When it is opened from the game,
  the hot key or the menu-bar icon it tries to take the keys at once (to be checked in the M4
  testing; `--overlay-focus click` waits for a click on the panel instead); the overlay shown at
  start-up gives them back if starting Curia took them. A menu-bar icon offers "Show Curia",
  "Hide Curia" and "Quit Curia" (not created if CK3 is in front when Curia starts). `--window plain` opens a normal
  window instead. Curia never asks for Accessibility, Input Monitoring, Screen Recording or the
  microphone.
- On Windows and Linux the chat opens in a normal window that stays on top of other windows (no
  hot key yet: click Advisor in the game); Esc minimises it and its close button quits Curia. A
  tray icon has the same three entries on Windows; Linux has none yet.
- `Curia.app` (macOS) next to the `curia` binary in the build folder: start it with
  `open -g build/macos-arm64/app/Curia.app` (see the README for how the development variables
  reach it).
- The app notices whether CK3 is running (checked once a second on macOS and Windows, every five
  seconds on Linux) and says so.
- `tools/ci/release_blockers.sh` lists the spike and development parts that must be removed before
  a release; CI runs it as a warning.
- A parchment look with a bundled font, Gentium Book Plus 6.200 by SIL International (SIL Open
  Font License 1.1), embedded unmodified in the app; its licence is in the generated third-party
  notices.
- `curia --smoke` draws a canned conversation (no network, no key, no game) and checks that the
  font has every character shown; `--screenshot <file.bmp>`, `--window-size <W>x<H>` and
  `--smoke-scene notices|start` (the notices over the conversation, or the empty window) go with it.
- `--effort default|low|medium|high|xhigh|max` and `--thinking adaptive|between_tools|disabled` for
  `--headless-chat` and `--check-api`: the request's effort and thinking settings (what is sent
  without them is under "Changed"). The headless `ready` and `done` events, the transcripts and
  the `CHECK_API_RESULT` line report them; `done` also gives `thinking_started_ms`, and the `status`
  event `thinking` comes once per answer, before its first word.
- The quality table (`tools/m4/quality_table.py`) has the columns Run (the file's label), Setting
  and Words, and its summary gives the median times and words per run label and setting.
- Headless modes of the `curia` executable (no window): `--headless-chat` reads the game's log, keeps
  the newest snapshot (or `--frame <snap>`) and answers questions typed on stdin, printing one JSON
  object per event; `--check-api [--cache-probe]` is the connection check for the owner (key, model,
  certificates, sign-in header, one tiny answer, prompt cache). Development builds only for the key
  from the environment; development transcripts (`CURIA_LOG_CONTENT=1`) go outside the repository.
  End-to-end tests `chat_e2e` and `e2e_secrecy` run the whole chain against the mock server.
- Design documents under `docs/`: requirements, architecture, milestones and
  research notes with sourced facts, assumptions and open questions.
- Milestone 0 feasibility findings (`docs/spikes/`), measured by hand on
  macOS 26.5.1 with Crusader Kings III 1.20.0.3. Nothing from this milestone is
  shipped. Main results: `debug_log` writes to `debug.log` on a normal launch,
  also in Ironman, with achievements still shown as available; a launch with
  `-debug_mode` showed achievements as not available; names, numbers and
  opinions resolve inside `debug_log` with `THIS`-based forms. (These were
  feasibility tests with a throwaway mod, `mod/curia-m0/`, which is not the
  Curia mod and must not be installed.)
- Build skeleton: CMake presets and a pinned vcpkg manifest, an SDL3 + Dear ImGui
  window with an event-driven loop, a thin platform layer, unit tests (doctest),
  a headless smoke test, an idle test and an HTTPS smoke test.
- CI on Windows, macOS (arm64 and a cross-built x86_64 merged into a universal
  binary) and Linux, with lint, sanitizers and the `CI result` aggregate check.
- Repository standards: `README.md`, `LICENSE` (MIT), `CONTRIBUTING.md`,
  `CODE_OF_CONDUCT.md` (Contributor Covenant 3.0), `SECURITY.md`, `SUPPORT.md`
  and this changelog.

### Changed

- The advisor asks for effort low and sends no thinking setting unless told otherwise (D22, from the
  owner's second quality check: the first word after about 1 s instead of about 6 s, about 60% fewer
  output tokens, rated as good as the default, average 4.4 against 4.6). `--effort default` sends no
  effort setting, and `--thinking` alone now comes with effort low; the reports say
  `default` for a setting that is not sent, and `--check-api` says whether the effort it sends is
  Curia's default or a flag.
- The advisor's wording after the second quality check (persona version 4): answers under 120 words
  unless the player asks for more, ending with a one-line offer when more would help; names and
  titles without quotation marks; "from what I can see" at most once per conversation, never as a
  fixed opening. The block the model reads about the realm says that the player can see each
  faction's members in the Factions window, and marks a faction's leader, claimant or target who is
  not among the listed vassals. `--check-api`'s advice for a refused request speaks only of the kind of
  setting that was sent.
- The advisor's wording, after the owner's first quality check (`docs/spikes/m4-quality-results.md`):
  plain words for the player ("from what I can see", never "snapshot" or "export"), about 120 words
  unless the player asks for more, a numbered list and `**bold**` as the only formatting, general CK3
  mechanics allowed
  when marked as general, and "probably" when inferring. The block the model reads about the realm is
  headed "WHAT THE ADVISOR CAN SEE" and uses the same plain words, as do the freshness note and the
  chat divider ("Updated from the game: DATE").
- The Curia mod (`mod/curia/`, now 0.2.1) writes the approved `CURIA1` export
  format of `docs/wire-format.md` instead of the spike layout: records with
  ids, section headers with totals, numeric date and mod version in `BEGIN`,
  the net monthly balance (income minus expenses, approximately as the top bar
  shows it, ±0.2) in tenths of a gold piece, explicit claims marked `strong` or `weak`, and
  faction groups. The format was frozen on 2026-10-11 after the pre-freeze
  in-game check, and the earlier spike output is not readable by the new
  format. `tools/ci/lint_mod.py` now checks the format's invariants.

<!--
Compare links are added when the first tag exists, for example:

[Unreleased]: https://github.com/cupskeee/curia/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/cupskeee/curia/releases/tag/v0.1.0
-->
