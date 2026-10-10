# Milestone 3 results

Run by the owner on macOS 26.5.1 (arm64) in three parts, with the real spike mod `mod/curia/`, the installer `tools/m3/install_mod.sh` and the log watcher `tools/m3/logwatch.py`. The session sheets are `docs/spikes/m3-protocol.md`, `docs/spikes/m3-quickrun-session1.md` and `docs/spikes/m3-session2-checklist.md`. Raw outputs stay with the owner (`~/curia_m3_results/`, not committed: they hold engine log text, local paths and game names).

| Part | Date | Game | Setup |
|---|---|---|---|
| Session 1 | 2026-10-06 evening to 2026-10-07 00:25 (owner's local time) | CK3 1.20.0.3 was updated by Steam to **1.20.0.4** before the launcher was opened, so every launch of the session ran 1.20.0.4; the launcher showed no warning for the `1.20.*` mod | Python 3.10.16, two displays, "Displays have separate Spaces" on, Stage Manager off, Fullscreen. Watcher runs: launch 2 (32.5 min) and the save/load watch (221.8 s) |
| Re-check | 2026-10-10, after PR #11 (`9daf1e7`) | 1.20.0.4 | Same Mac, **single screen** (laptop only; the owner reached the terminal window with Cmd+Tab and checked it after each group of clicks rather than watching live). Watcher runs: a throwaway save of realm 1 (labels paused, normal) and a new throwaway game with a small realm (label small) |
| Session 2 | 2026-10-11, checklist `m3-session2-checklist.md` | 1.20.0.4 | Same Mac, **single screen**; launch type not recorded in the session notes. Install check before the session: the `claim_detail` count was 7 in the installed mod and 2 in the repo copy, as the checklist expects; no launcher warning. Display mode: not recorded in the session notes. Four watcher runs: labels `ironman`, `claims_king`, `factions_a`, `factions_b` |

Sources used below: *owner* = the owner's observation; *log* = the watcher files (`summary-*.txt`, `watch-*.jsonl`, `raw-curia-*.txt`, checked by the maintainer's tooling against the owner's result files); *game files* = installed files read by the maintainer. Realms are described generically: **realm 1** = a king with 11 vassals in the Vassals window, one war, one claim and one heir (a kingdom start; the throwaway save `curia_m3_throwaway` of session 1, reloaded in the re-check and again in session 2); **realm 2** = a petty king with one vassal, no war, one claim and one heir (the re-check's new game); **realm 3** = a petty king with one vassal, one claim and an heir with three claims (session 2's new game, **Ironman on**, a 1066 start; its counts match realm 2, but whether it is the same start is not established); **realm 4** = a duke who is the vassal of a king (session 2's second new game, a 1066 start, no war, no claim). No game text, names or raw log lines are kept in the repository.

**Scope notes.** Only macOS was tested (owner, arm64, macOS 26.5.1). Fullscreen in session 1 and the re-check (session 2's display mode was not recorded); Windowed is deferred to the M4 testing, as for M2. T8 per hour of normal play, T11 beyond the default-install row and the 17MB cap (C3) were deferred by the owner on 2026-10-06 and are not in this record. Observer mode, loading screens, the pause-menu T3 state and OS file-change notifications (A15) were deferred by the owner on 2026-10-10 and stay open. T9 was optional and was not run.

## Verdict against the acceptance criteria (milestones.md M3)

| # | Criterion | What this record answers | Verdict |
|---|---|---|---|
| 1 | Every T-question has a recorded answer, none estimated | T3, T4, T5, T7, T8 (bytes per export only), T10, T11 row A and T12 answered below with date, game version and OS; T9 not run; A15 (OS file-change notifications) not tested because the watcher polls (deferred by the owner, 2026-10-10). Session 2 (2026-10-11) added the Ironman button check, the claims filters and the faction-scope forms (section "Session 2" below). The fold-back into `research-notes.md` is done (§2.16 and §5.1 hold it, including the session 2 additions) | **answered in part**: the T3 menu state (deferred), T4 quotes/braces/newlines and the T7 states not tried (map view variants, observer mode, loading screens, event windows) are open, T8 per hour and T11 beyond row A are deferred, T9 and A15 not run/tested; open items from session 2 are listed under "Deferred or still untested" |
| 2 | The button produces a marker; latency distribution measured for pause, menu and fast-forward | The button produced 70 complete frames in the session 1 launch 2 watch, 1 in the save/load watch and 20 in the re-check, which the watcher detected; session 2 added 6 complete frames (Ironman 2, claims 2, factions 1 + 1), the Ironman ones from a game with Ironman on. Latency was measured for paused, after_unpause, normal (speed 3) and fast (speed 5): across 2524 samples in eight watcher runs (1703, 18, 410, 46 in session 1 and the re-check; 60, 128, 82, 77 in session 2) no line was seen more than 1.023 s after the start of its engine second (the session 2 maximum was 0.956 s; the state at each session 2 click was not tallied). **Menu state: the buttons are not clickable with the pause menu open (0/12 Advisor, 0/4 probes), so there is no menu sample.** Detection by the app itself is Phase 2 | **answered in part**: game side measured for paused, after_unpause, normal and fast; menu has no sample by construction; app-side detection is Phase 2 |
| 3 | Trigger decision (D2) confirmed or revised with the data | The log marker arrived inside its own engine second in all but one frame (see T3); none of the 347 session 2 samples was seen after the end of its second. T9 (clipboard) was not run, so there is no clipboard measurement to weigh against it. The data gives no reason to revise D2; confirmation belongs to the wire-format checkpoint with the owner | **data supports D2; confirmation at the wire-format checkpoint with the owner** |
| 4 | Wire format frozen as `CURIA1` in `docs/wire-format.md` | Inputs recorded here: line shape and sizes (T4), bytes per export (T8), value forms, the gaps listed under "Proposals" and the facts under "Inputs from session 2 for the wire-format checkpoint". Nothing is frozen | **Phase 2** |
| 5 | Parser tests on real fixtures, fuzz in CI, fake CK3, end-to-end CI test | Not started. This record gives the captured structure (engine prefix, interleaved non-Curia lines, truncation at launch, UTF-8 names). Session 2 adds frames that contain factions (77 and 81 lines), a line with control bytes inside link markup, and a repeated `LOAD` line; whether these enter the fuzz corpus and fixtures is a Phase 2 decision. The fake CK3 reproduces that structure with a **synthetic** engine-style prefix and interleaved lines; whether the fixtures keep a (synthetic) prefix is a Phase 2 decision, since `architecture.md` says committed fixtures hold only Curia marker lines | **Phase 2** |
| 6 | Mod loads with no `curia_` entries in `error.log`; ck3-tiger reviewed (T12); T11 documented in M7 | Session 1: the mod loaded, but each export wrote a Curia range error (claims error, below). After PR #11 the re-check wrote none: realm 1, 17 exports, `error.log` +3,562 B; realm 2, 3 exports, +15,474 B; no Curia entry in either. Session 2 (realms 3, 1 and 4, including an Ironman game): `errcount` stayed 0 for the whole session, also after every new claim and faction probe, and the watcher's own `error.log` section listed no Curia line in any of the four runs; `error.log` grew by 58.7 KB, 0.9 KB, 11.8 KB and 70.7 KB in the four runs (in run order), none matching `curia`. ck3-tiger: 0 findings. T11 beyond row A: M7 | **pass after PR #11 (macOS, realms 1 to 4, one of them Ironman)**; T11 in M7 |

## Results per question

### T10. `script_docs` and `dump_data_types` on the Mac (launch 1, throwaway game)

| Item | Result |
|---|---|
| Debug mode in the launcher | A **one-shot launch button** next to "Open game in Debug Mode" (Game Settings, Game tab), not a toggle: nothing to switch off afterwards. This corrects the M3 sheets and the M0 phrase "debug mode setting" (*owner*) |
| Console | Opens in the debug launch. In a **plain launch** one press of the console key opened nothing (*owner*). So the console exists only in a debug launch; whether the two commands themselves need debug mode cannot be tested without a console |
| `script_docs` | Replied at once with a line naming the `logs/` folder, no freeze (*owner*). 7 files written there: custom_localization 856,362 B, effects 647,817, event_scopes 8,898, event_targets 53,914, modifiers 45,884, on_actions 84,535, triggers 411,780 (*game files*, sizes) |
| `dump_data_types` (this spelling) | Replied at once with a line naming `logs/data_types/`, no freeze. 5 files: common 41,208 B, gui 91,864, internalclausewitzgui 264,115, script 98,395, uncategorized 2,184,143 |
| Handling | Copied to the gitignored `reference/ck3/` (`git check-ignore` confirmed). Never committed |
| Achievements | In the debug launch the indicator read "Available" at the setup screen and "Unavailable" after the game started (*owner*). M0's debug launch read "Not available" right away on a different bookmark; the cause of the difference was not isolated |

Not shown: whether a console command used in a plain launch is possible by any other route (not tested; Curia never asks players to use the console, hard rule 7).

### T5. `debug.log` and `error.log` at launch (launch 2)

| Item | Result |
|---|---|
| Behaviour | Both logs were **truncated in place at game launch**: same inodes before and after, size dropped to 0 bytes at one instant about 34.8 s after the watcher attached, no new file, no BOM, 0 unterminated bytes discarded (*log*) |
| Consequence | The M0 note "debug.log is recreated at launch" is **not supported**. A tailer must handle a size drop to 0 on the same inode (research-notes A4) |
| Start-up volume | At the main menu `debug.log` held 442,669 B and `error.log` 159 B. A new game added about 570,000 B in this session (largest single minute 570,350 B) |
| LOAD line | The 5-field `LOAD` line (`mod`, `version`) appears once, at a **new game** (on_game_start_after_lobby). None after loading a save. Session 2: one `LOAD` line at the new game of the `factions_a` run, but **two** in the `ironman` watch, 40 s apart; the watcher flagged the second as a duplicate line, and its snapshot id 0 made it a "frame 0". The cause is not established (the owner's notes do not mention starting a game twice). A `LOAD` line may repeat, so nothing may depend on seeing it exactly once |
| Session 2 | No truncation, re-creation or disappearance of `debug.log` or `error.log` in any of the four runs |

Not shown: behaviour on Windows or Linux; a launch with a very large existing log; how the game handles two game instances.

### T4. Line format, length, charset

| Item | Result (session 1 and the re-check: 2177 CURIA lines in four watcher runs, 1703 + 18 + 410 + 46; session 2 figures are marked and come from four more runs with 347 CURIA lines, 60 + 128 + 82 + 77) |
|---|---|
| Shape | Bracketed time, level, engine source file and line, then `file: <script path> line: <n> (<effect name>): ` and the text. The shape was the same for all 2177 lines; the example pattern is in research-notes §2.14 and no real line is reproduced here. Session 2: the engine source file and line part was still the same one for every line |
| Engine prefix before the marker | min 119, median 121, max 124 bytes; it varies with the script path and effect name length. M0's "about 110" was for shorter names. Session 2: 119 to 131 bytes |
| CURIA line length | 134 to 199 bytes over all runs (session 1 launch 2 watch 135 to 199, save/load watch 136 to 198, realm 1 re-check 136 to 198, realm 2 run 134 to 194). Session 2: the longest was **321 bytes** (without terminator, engine prefix included): the faction name line, which carries link markup (see "Session 2", factions); the longest line of the realm 4 run was 188 bytes |
| Longest line anywhere | None over 4000 bytes (13,878 other lines checked in session 1, 525 in the realm 1 re-check, 5,339 in the realm 2 run) |
| Encoding | UTF-8 throughout, no BOM, no CRLF. Names with non-ASCII characters (accented Latin, eth, thorn, o-umlaut) are valid UTF-8 (19 lines in the first two snapshots); the 647 `non_ascii_or_control` anomaly records of session 1 are these and are informational. Session 2: UTF-8 names (o-umlaut, thorn, eth) passed through unchanged in vassal, heir, leader, target and title-holder names. **New: one line type carries control bytes** (0x15, inside link markup in the faction name; see "Session 2", factions), which the watcher flagged as a control-byte anomaly |
| Field separator | `\|` survives. In session 2 the faction name markup contained `;`, `,` and `:` but no `\|` in the one sample seen; whether a name can ever contain `\|` is not established |
| Frame integrity | The END line's count equals all lines of the frame including BEGIN and END in 70 of 70 complete frames of the launch 2 watch, in the 1 frame of the save/load watch and in all 17 + 3 frames of the re-check. Session 2: the two Ironman frames (7 and 51 lines) are complete with the END count equal to all lines including BEGIN and END, span 0.000 s, 0 interleaved lines; the realm 1 faction frame (77 lines) has span 0.000 s and 0 interleaved lines; the realm 4 frame (81 lines) is complete |
| Interleaving | The engine writes multi-line entries (an error entry is a header, the error line, the script location and two caller lines, then a blank line: 6 lines). In session 1 these sat **inside** frames (6 non-Curia lines per frame, "interleaved 6/0"); in the re-check, whose exports were free of Curia errors, "interleaved 0/0". A parser must tolerate non-Curia lines between frame lines |
| Snapshot numbering | `curia_snap_id` is a global variable stored in the save: save at 70, load, next export 71 (seen twice). A **new game restarts at 1** (realm 2: 1, 2, 3; session 2: realm 3 snapshots 1 and 2, realm 4 snapshot 1). Because LOAD appears only at a new game, a reload is not announced; an older save reloaded brings the number back down or repeats it. Session 2 saw it again: realm 1 was loaded from the save at 70 twice (the `claims_king` and `factions_b` runs), the first showing 71 and 72 and the second 71 |

Not tested: quotes, braces or newlines inside exported text; lines longer than M0b's 8,000-character single-line test (the longest CURIA line so far is 321 bytes).

### T3. Flush latency

Method: the watcher polls every 10 ms, reads appended bytes and stamps arrival. Each CURIA line carries the engine's whole-second clock, so each sample is bracketed: lower bound = arrival minus (engine second + 1), upper bound = arrival minus engine second.

| Run | Samples | Seen after the end of their engine second | Upper bound max | Notes |
|---|---|---|---|---|
| A (session 1, launch 2 watch) | 1703 | 0 | 0.982 s (lower bound max -0.018 s) | Confounded by the claims error (below). 0 lines needed more than one read; every label alike (paused, after_unpause, normal, fast); the menu label has no samples |
| A2 (session 1, save/load watch) | 18 | 0 | 0.495 s (lower bound max -0.505 s) | One frame, label `saveload`; the same session 1 error-level block applies |
| B (re-check, exports free of Curia errors) | 410 (196 paused, 214 normal) | 18, all the 18 lines of **one frame** (snapshot 83, label normal), seen 0.023 s after their second ended | 1.023 s | See the late frame below |
| B2 (re-check, realm 2) | 46 | 0 | 0.736 s | |
| C (session 2, four runs: `ironman` 60, `claims_king` 128, `factions_a` 82, `factions_b` 77) | 347 | 0 | 0.956 s over the four runs (`ironman` alone 0.951 s, lower bound max -0.049 s) | Single screen again; samples are CURIA lines, `LOAD` lines included. The state at each click was not tallied |
| Total | 2524 | 18 (one frame) | 1.023 s | |

**Run A confound.** Every export in run A also wrote an error-level block into `debug.log`, between the WAR and CLAIM lines. The game's log settings (game files, research-notes §2.13) have `always_flush_level: error` and `flush_interval_seconds: 3`, so an error-triggered flush could have forced the lines out. The CLAIM and END lines were written **after** the error and still arrived within their second in every frame, so an error flush alone does not explain run A; but every run A export carried a Curia error block, which is why run B was made (after PR #11 removed it).

**The one late frame in run B.** Snapshot 83 (label normal) was seen 0.023 s after its engine second ended. The file's modification time recorded at that read was 0.064 s before the poll saw the data and still inside the stamped second (the modification time read ...20.959, the arrival ...21.023). That is consistent with a late poll rather than a late write, but it is not proof: the watcher reads the modification time before it reads the data, and no record of the gaps between polls exists. The cause is not established. A guess, untested: the terminal window was not frontmost on a single screen. It is not read as a game-side delay.

**What run B does and does not exclude.** The re-check exports were free of **Curia** errors only. In the same window the **game's own** error-level lines were written to `debug.log` (the first in the same second as snapshot 80, after its END; further ones followed, one burst of 8 entries among them), and `error.log` grew by 3.5 KB with no Curia entry. So the normal-speed samples still sat among the game's own error-level lines and run B is not an error-free control for them. The part that is clean is the 8 paused frames (196 samples): no error-level line in `debug.log` for at least 33 s before them. The one late frame (snapshot 83) came 10 s after the last such error line, so an error flush does not explain it either.

**Phase coverage was not controlled.** The `fast` label has samples only in session 1 (run A); the re-check had paused and normal only. In session 1 the owner clicked faster than the sheet asked (see T7), so the phase of each click within the engine second was not controlled or recorded as a design variable. The brackets of every sample stay valid, but how well the samples cover the sub-second phases of the engine second is not known.

**Session 2 runs.** The four runs of 2026-10-11 repeat the re-check's setup (single screen) with the probe build and found no sample after the end of its engine second; the late frame of run B did not recur. They do not change the open phase-coverage and menu-state limits below.

**Supported conclusion.** Across 2524 samples no line was seen more than 1.023 s after the start of its engine second, and there is no sign of the 3 s flush interval. Sub-second latency cannot be resolved with whole-second engine stamps; zero latency is not excluded for any sample. Measured states: paused, after_unpause, normal (speed 3), fast (speed 5, run A only). Menu state: not clickable, not measured.

Clock side-note (not interpreted): across all prefixed lines, floor(arrival minus engine second) had median 0 s and max 316 s in the session 1 launch 2 watch (12,512 lines) and max 288 s in the save/load watch (510 lines); in the re-check median 0 and max 33 s over 675 lines for realm 1 and max 51 s over 5,034 lines for realm 2 (median 0), for some non-CURIA lines. CURIA lines are inside their second.

Not shown: the pause-menu state (deferred by the owner, 2026-10-10); whether the OS notifies file changes (A15, also deferred), so an event-driven watcher is not proven to be faster or equally prompt; whether Windows and Linux behave alike; the effect of a much busier game (late-game, many characters) on flush timing.

### T7. The real buttons in game (Fullscreen)

| Item | Result |
|---|---|
| Visibility | Both HUD buttons ("Advisor", "Curia probes") sit at the bottom of the screen, right of the ruler section, overlapping neither the HUD nor each other. Hover highlight, tooltips as written (*owner*) |
| Scope | The click runs as the player: BEGIN name = the ruler, BEGIN date = the HUD date, FIN gold = HUD gold (481 = 481) (*owner*) |
| Income | HUD +7.8 per month; the probe prints 8. The `\|0` format rounds to an integer; a decimal form was not tried |
| Click protocol (session 1, 70 exports) | Before the protocol: one Advisor and one probes frame (snapshots 1 and 2, 37 s apart, label `before_launch`). Then paused 12 Advisor + 4 probes (12/12, 4/4), after_unpause (tally 12 + 4, 16 Advisor + 4 probes frames logged), normal speed 3 12 + 4, fast speed 5 12 + 4. All produced complete frames. No stutter, popup or sound noted (*owner*) |
| Pause menu (Esc) open | **The buttons are not clickable** (0/12, 0/4) (*owner*). The "click then Esc quickly" fallback was not run, so whether a quick click-then-Esc registers is unmeasured |
| Frame span | Every frame's lines arrived within 0.016 s of each other (span 0.000 to 0.016 s): an export is a single burst. Inference: a snapshot cannot be half-written by opening a menu after the click; the fallback itself was not run |
| after_unpause 16 vs tally 12 | The 16 Advisor frames are snapshots 19 to 34 and the 4 probes frames are 35 to 38; 19 to 34 are consecutive, no duplicate numbers, smallest gap between consecutive frames 1.42 s. Cause (extra clicks vs the Space key re-triggering) unresolved; no evidence of doubled frames at sub-second gaps |
| Click rhythm | The owner clicked faster than the sheet asked (median gap 0.54 s paused, 2.5 s after_unpause, 1.5 s normal, 1.1 s fast, instead of irregular 4 to 10 s). The T3 brackets stay valid (they depend on the engine second), but the phase coverage of the engine second was not controlled (see T3) |
| Background play | The game keeps running while another app is frontmost: the date advanced while the owner typed. Curia cannot pause the game (read-only) |
| Re-check (2026-10-10) | Realm 1 save: paused 6 Advisor + 2 probes frames (tally 6/6 and 2/2); normal 7 Advisor frames (snapshots 79 to 85) + 2 probes (tally 6/6 and 2/2, so one Advisor frame more than tallied, cause not established). Realm 2: 2 Advisor + 1 probes frames (tally 2/2 and 1/1) plus the LOAD line |
| Ironman (session 2, realm 3) | Both buttons visible and clickable; one Advisor click and one probes click (the checklist asked once each) each produced a complete frame; no popup (*owner*, *log*) |
| Probe clicks in session 2 | The probes button produced a complete frame each time it was clicked in realm 3, realm 1 (three probe frames: two in the claims check, one in factions path B) and realm 4 (the click registered in each; frames in the Session 2 table below) |

Not tested: map view variants, observer mode and loading screens (the last two deferred by the owner, 2026-10-10), Windowed (deferred to the M4 testing), event windows during clicks (none noted), repeated clicks in an Ironman game (one of each only).

### T8. Bytes per export (this build, these realms; per hour is deferred to M4 criterion 8)

| Export | Lines | CURIA text | In the file, with engine prefixes |
|---|---|---|---|
| Realm 1, Advisor (10 exported vassals, 1 war, 1 claim, 1 heir) | 18 | 911 to 929 B (the first frames 911 B; later 926 to 929 B because values changed) | 3124 to 3142 B |
| Realm 1, Curia probes (the 18 plus 26 probe lines) | 44 | 1808 to 1859 B | 7170 to 7221 B |
| Realm 2, Advisor (1 vassal, no war, 1 claim, 1 heir) | 7 | 230 B | 1084 B |
| Realm 2, Curia probes | 31 | 1031 B | 4794 B |

Size scales with: vassals (capped at 10), claims (capped at 10), wars (3 lines each, **not capped**) and the fixed lines (BEGIN, FIN, HEIR, END = 4, plus the WAR and CLAIM `none` lines when empty). The uncapped wars are a gap for the wire-format freeze (noted, not decided). Session 2 sizes (Ironman Advisor and probes frames, and probe frames with factions of 77 and 81 lines) are in the "Session 2" section, "Frame sizes".

By-product, not normal play and not an input to the freeze (all sizes in bytes, as the watcher counted them): session 1's launch 2 watch grew `debug.log` by 1,720,594 B over 31.6 active minutes (CURIA lines 289,133 B, the game's own 1,431,461 B, of which 442,669 B came before the main menu and 570,350 B in the single minute of the new game) and `error.log` by 112,006 B. Of that `error.log` growth, 71 entries (432 B each, 30,672 B) were Curia's claims error; the rest were the game's own errors (for example a character-scope error and DLC-locked tenets). The realm 1 re-check window grew `debug.log` by 116,971 B in 2.0 active minutes (CURIA lines 69,704 B) and `error.log` by 3,562 B with no Curia entry. The realm 2 run grew `debug.log` by 584,093 B (CURIA lines 7,119 B, the new game's burst included) and `error.log` by 15,474 B, also with no Curia entry.

Session 2's byte-budget windows were all shorter than 10 minutes, so they give no per-hour figure either. Not measured: bytes per hour of normal play (M4 criterion 8), the effect of heavy late-game realms on frame size, and the 17MB cap (C3).

### Value forms (export and probe section)

| Item | Result |
|---|---|
| Export | Everything printed sensible values with no error: names, date, gold, heir (name, opinion of the ruler, has liege, landed), vassal list (rank, name, title, gold, opinion), wars (name, attacker, defender; none prints `WAR\|0\|none`), claims (`CLAIM\|n\|<title>`) |
| Probe lines | 26 lines in a realm with a war and a vassal printed values with no `error.log` entry: snapshot id from `THIS`, character id, first name, monthly and yearly income, military strength, realm size, landless flag, vassal count three ways, claim counts for ruler, heir and vassal, vassal id, name, landed and strength, claim tier, war score, start date and enemy (the war probe printed `war\|none` in the realm with no war) |
| Cross-checks against the UI | Session 1 and the re-check: only name, date, gold, vassal counts and the claim count in realm 1. Session 2 added the ruler's claim count and its pressed/unpressed kind (realms 1 and 3) and, for the claimant faction of realm 1, kind, power, members, leader, target and discontent (see below) |
| **Faction-scope forms** | In session 1 and the re-check `faction\|none` only: no faction existed in either realm. **Session 2: they ran** in three factions across two games (a claimant faction against the ruler of realm 1; a peasant faction against the ruler and the independence faction the ruler led, in realm 4) and printed values with no `error.log` entry. Details below |

#### Value forms proven or changed in session 2

| Form | Result |
|---|---|
| `ordered_claim` with explicit and pressed filters | All nine explicit/pressed pairs (each of yes, no, all) ran for the ruler and the heir with no error; the counts are in "Session 2", claims |
| `every_claim` counts | `ruler_claim_count` and `heir_claim_count` printed; each equals the all/all filter count in realms 1 and 3 |
| Claim kind flags (`claim_detail`) | The four flag triggers (strong and weak, explicit and implicit) ran inside the claim iterator on a saved temporary scope and printed 0 or 1 with no error. The two implicit flags were 0 in every claim seen, so their 1 case is unproven |
| Faction iteration | Iteration over the factions that target the ruler (`faction_source\|targeting`) and the faction the ruler has joined (`faction_source\|joined`; `joined_faction`) ran. In realm 1 the joined-faction probe printed `none` for the ruler (no faction of its own); in realm 4 it printed the independence faction the ruler led |
| Faction fields | Type code (1 independence, 3 claimant and 5 peasant seen; 2, 4, 6, 7 never produced), power in two forms (a data-function form `_df` and a script-value form `_sv`), power threshold, discontent in two forms, member count, at war, can-press-demands, dangerous (a data-function bool turned into 1 or 0 with `Select_CString`), leader and target (character `GetNameNoTooltip`, or `none`) all printed values |
| Faction name | The localized name in the `GetNameNoTooltip` form **still carries link markup**: the control byte 0x15 opens and closes tags (`ONCLICK`, `TOOLTIP`, with an id, and `!` closers) around the character and title names, and the text contains `;`, `,` and `:`. Leader and target names are clean. See "Session 2", factions |
| Formats | Power is printed with the integer format `\|0`; the discontent data function with `\|2` printed `0.00` against the script value's `0`. Why the power in the Factions window differs from the probe by one is not established (see "Session 2", factions) |

### Vassal counts (realm 1)

| Source | Count |
|---|---|
| Vassals window | 11 |
| `every_vassal` iterator | 11 |
| `vassal_count` trigger | 9 (the game's trigger documentation says it excludes barons) |
| `every_vassal_or_below` | 17 |
| Exported VASSAL lines | 10 |

The difference is by design and by definition, not an error: the export keeps the strongest 10 vassals by military strength (`curia_max_vassals` = 10), so the weakest of 11 was not exported; the trigger counts a narrower set; "or below" includes vassals of vassals. The frame has **no total-vassal-count field**, so a capped list cannot be told from the whole realm (gap for the freeze). Realm 2 (1 vassal): trigger 1, iterator 1, below 2, one VASSAL line.

### T11 row A. Default-install launcher check

The installer wrote 11 files under `curia/` plus `curia.mod`. The launcher listed Curia by itself when adding mods, enabled, with no warning text. Afterwards only `curia.mod` differed: 156 bytes (the installer's file ends with a newline) became 155 bytes (the launcher removed only the final newline; the installer's checksum equals the pre-launcher checksum, and the installed copy under `curia/` was unchanged). The mod loaded: the LOAD line appeared, and `error.log` had no Curia entry from loading. The variants (`supported_version` values, which of `descriptor.mod` and `.metadata/metadata.json` the launcher needs) stay in M7 criterion 10.

### T12. ck3-tiger (local only)

Built with cargo on the Mac and run with `--game <install path>` (automatic install detection was not tested): **0 findings** (fatal, error, warning, untidy and tips all 0) on `mod/curia`, with a banner that tiger targets 1.19.0 while the game is 1.20.0.4. Tiger does not and cannot catch the runtime range error described next.

### Achievements

Plain launch with the mod: "Available" at the setup screen, in the pause-menu icon and in Game Rules, before and after the click session and after save and load (*owner*). In the **Ironman** game of session 2 (realm 3) achievements read "Available" with the Curia buttons clicked (*owner*). The debug launch is under T10.

### Save and load

Saving after clicks worked; loading worked; an Advisor click after loading produced a frame (18 lines); the snapshot number continued (T4).

### T9 (clipboard comparison)

**Not run** (optional in the milestone). No clipboard latency or behaviour is claimed anywhere in this record.

### A15 (OS file-change notifications)

**Not tested.** The watcher polls every 10 ms; whether file-change notifications fire for `debug.log` on macOS, Windows or Linux is unknown.

## The claims error and the PR #11 fix

- **What happened.** In session 1 every export wrote an error-level entry into `error.log`, and the same block into `debug.log` between the WAR and CLAIM lines. `error.log` held 71 Curia entries of 432 B each (30,672 B), with 71 exports run in the session (the 70 watched plus the one after loading); the match is observed, not separately proven.
- **Cause.** The iterator `ordered_claim` was called with `max` larger than the list, and the engine reports a range error once per call. All 71 entries in session 1 name `ordered_claim` in the claims section of the export (realm 1 held one claim against a `max` of 10); none names the vassal list (*log*). That is expected: realm 1 had 11 vassals against a `max` of 10, so the vassal iterator never ran past its list. The `check_range_bounds = no` setting on `ordered_vassal` (below) is therefore a precaution: no vassal range error was observed in session 1, and it would error only in a realm with fewer than 10 vassals, which session 1 did not have. The exported values were still printed; the cost was an error entry per export and a non-Curia block inside every frame.
- **Why the static check missed it.** ck3-tiger found nothing; this is a runtime check that tiger does not make (T12 above).
- **Fix (PR #11, commit `9daf1e7`).** `check_range_bounds = no` on both iterators (the claims one removes the observed error; the vassal one is the precaution above). The key is used 131 times in the installed vanilla files but is not documented for these two iterators in the dumps, so the fix is evidenced by use in the game files and by the re-check, not by documentation.
- **Re-check outcome (2026-10-10).** `error.log` showed 0 Curia entries after the realm 1 run (17 exports) and after the realm 2 run (3 exports). A realm with 1 vassal and 1 claim gives exactly one VASSAL line and one CLAIM line and no error; this is the first run of the vassal precaution in a realm that could have triggered it, and it shows no error with the fix, not that the error ever occurred. Interleaved non-Curia lines inside frames dropped from 6 per frame to 0. The realm 1 re-check window grew `error.log` by 3,562 B with no Curia entry.

## Session 2 (2026-10-11): Ironman, claims, factions

Run with `m3-session2-checklist.md` in four watcher runs (labels `ironman`, `claims_king`, `factions_a`, `factions_b`), on the probe build, 1.20.0.4, single screen. `errcount` read 0 at every check of the session, after every new claim and faction probe included. The result files (`summary-*`, `watch-*`, `raw-curia-*` of each run, plus copies of `error.log` and `debug.log` taken before the next launch) stay with the owner. Realms are the ones defined at the top: realm 1 (the throwaway save, king), realm 3 (Ironman new game, petty king) and realm 4 (second new game, duke under a king).

### Ironman (realm 3)

| Item | Result |
|---|---|
| Setup | New game, Ironman on; one Advisor click, then one Curia probes click (the checklist asked for one each) |
| Buttons | Both visible and clickable; frames produced; no popup (*owner*) |
| Frames | Snapshot 1 = Advisor (7 lines), snapshot 2 = probes (51 lines). Both complete, the END count equals all lines including BEGIN and END, span 0.000 s, 0 interleaved lines (*log*) |
| Achievements | "Available" with the Curia buttons clicked (*owner*) |
| Run `ironman` | 60 CURIA lines = the 7 + 51 frame lines + 2 `LOAD` lines; 60 T3 samples, none seen after the end of its engine second (upper bound max 0.951 s, lower bound max -0.049 s) |
| Two `LOAD` lines | 40 s apart; cause not established (see T5). The watcher listed the second as a duplicate line |
| Names | UTF-8 names with non-ASCII letters passed through (vassal, heir, leader, target and title-holder names) |

### Claims (realms 3 and 1)

Character window (*owner*): the ruler of realm 3 lists **1 claim, unpressed**; the ruler of realm 1 lists **1 claim, pressed**. Neither ruler has a living parent who holds titles. The heirs' claims were not looked up in the UI.

Counts by `explicit` / `pressed` filter on `ordered_claim`, from the probe lines (*log*):

| explicit / pressed | Realm 3 ruler | Realm 3 heir | Realm 1 ruler | Realm 1 heir |
|---|---|---|---|---|
| yes / yes | 0 | 0 | 1 | 0 |
| yes / no | 1 | 0 | 0 | 0 |
| yes / all | 1 | 0 | 1 | 0 |
| no / yes | 0 | 3 | 0 | 0 |
| no / no | 0 | 0 | 0 | 0 |
| no / all | 0 | 3 | 0 | 0 |
| all / yes | 0 | 3 | 1 | 0 |
| all / no | 1 | 0 | 0 | 0 |
| all / all | 1 | 3 | 1 | 0 |
| existing `ruler_claim_count` / `heir_claim_count` (default filter) | 1 | 3 | 1 | 0 |

Realm 1's two probe frames (snapshots 71 and 72) are identical. The existing vassal claim probe printed 6 for a vassal of realm 1.

`claim_detail` (the ruler's claims only; fields: index, title, four flags explicit-strong, explicit-weak, implicit-strong, implicit-weak, then the tier, 4 = kingdom):

| Realm | Line (title as a placeholder) | Reading |
|---|---|---|
| 3 | `claim_detail\|1\|<title>\|0\|1\|0\|0\|4` | explicit, weak |
| 1 | `claim_detail\|1\|<title>\|1\|0\|0\|0\|4` | explicit, strong |

What the data shows:

- **Explicit pressed = strong, explicit unpressed = weak**, in both rulers; this matches the trigger names `has_strong_claim_on` and `has_weak_claim_on` in the dumps. Both flags agree with the character window (realm 3 unpressed, realm 1 pressed).
- **The heir's 3 claims in realm 3 are all implicit** (`explicit` = no) and are counted as **pressed** (`pressed` = yes). The per-claim kind of the heir's claims was not probed (`claim_detail` covers the ruler only), and the heir's UI list was not checked.
- **Default filter.** `ordered_claim` and `every_claim` with no filter match `explicit` = all, `pressed` = all in every case above; for realm 3 that pair is the only one that fits both the ruler's 1 and the heir's 3.
- **Not observed:** a ruler with an implicit claim (the implicit flags were 0 for both rulers); an explicit pressed claim on an heir; how many claims the iterator returns for a ruler with many (`curia_max_claims` is 10).

### Factions

The Factions window is in the right sidebar (*owner*). Both paths of the checklist were run.

**Path B, realm 1 (throwaway save, king).** Run at speed 5 until a **claimant faction against the ruler** appeared (game date 2 Nov 1071); the real minutes were not tracked, so the checklist's "N minutes" is missing. The save at snapshot 70 was loaded, so the frame is snapshot 71. Frame: 77 lines, longest line 321 B (the faction name line), span 0.000 s, 0 interleaved lines. The ruler had no faction of its own (`faction_joined` printed `none`).

| Field | Factions window (*owner*) | Probe (*log*) |
|---|---|---|
| Kind | claimant | `faction_type_code` 3 (claimant); matches |
| Power | 32% | `faction_power_df` 33 and `faction_power_sv` 33; **one higher than the window**, cause not established (the owner noted the game was running) |
| Power threshold | no window value noted | 100 |
| Discontent | 0% | `faction_discontent_df` 0.00 and `faction_discontent_sv` 0; matches |
| Members | 2 | 2; matches |
| Leader | a count | the count's name (`GetNameNoTooltip` form) |
| Target | the king (the ruler) | the ruler's name; matches |
| At war / can press demands / dangerous | no window value noted | 0 / 0 / 0 |

**The faction name carries link markup** even in the `GetNameNoTooltip` form. The line has the shape (`\x15` is the control byte 0x15; `<text>`, `<id>`, `<name>` and `<title>` stand for text that is not kept here):

`<text> \x15ONCLICK:CHARACTER,<id> \x15TOOLTIP:CHARACTER,<id> \x15L <name>\x15!\x15!\x15! <text> \x15ONCLICK:TITLE,<id> \x15TOOLTIP:LANDED_TITLE,<id> \x15L; <title>\x15!\x15!\x15! <text>`

- The plain name is recoverable by stripping the markup; the **claimant's character id is inside it**, so the claimant is recoverable from the name, but a dedicated field would not depend on that. The dumps list a faction link `special_character`, the trigger `has_special_character` and the data functions `Faction.GetSpecialCharacter`, `GetSpecialTitle` and `HasSpecialCharacter` (`event_targets.log`, `triggers.log`, data types); **none of them has been run in the game**.
- The markup holds `;`, `,` and `:` but **no `|` in this sample**; whether any name can contain `|` is not established.
- The watcher flagged the line as a control-byte anomaly. Leader and target names (character `GetNameNoTooltip`) were clean.

**Path A, realm 4 (duke under a king).** The owner created an independence faction in the game; the click registered. Frame: 81 lines, longest line 188 B, complete. The realm had no war and no claim (`WAR|0|none`, `CLAIM|0|none`, `ruler_claim_count` 0, `claim_detail|none`) and had an heir. Two factions were printed (probe values; the window was not compared for this realm):

| Field | `faction_source\|targeting`: peasant faction against the ruler | `faction_source\|joined`: independence faction the ruler led |
|---|---|---|
| Type code | 5 (peasant) | 1 (independence) |
| Power (`_df` / `_sv`) | 170 / 170 | 30 / 29 |
| Power threshold | 0 | 80 |
| Discontent (`_df` / `_sv`) | 0.00 / 0 | 0.00 / 0 |
| Members | 0 | 1 |
| At war | 0 | 0 |
| Can press demands | 0 | 1 |
| Dangerous | 0 | 1 |
| Leader | none | the ruler |
| Target | the ruler | the liege (a king) |

What the faction probes show and do not show:

- First time a peasant faction and a power above 100 were seen. A peasant faction printed `none` for its leader and 0 members, so those fields can be empty.
- Between the two power forms: equal for the claimant (33 and 33) and peasant (170 and 170) factions, **one apart** for the independence faction (30 and 29). Against the window the claimant value was one lower there (32 against 33). The cause is not established.
- **Discontent was 0 in every faction seen**, in both forms; a nonzero discontent never ran.
- `can_press_demands` was 1 for the independence faction at power 30 against a threshold of 80, and 0 for the claimant faction at 33 against 100: it is **not simply power at or above the threshold**. `dangerous` was 1 for independence and 0 for claimant and peasant. The meaning of both is **not established**.
- Three identical PROBE lines appeared across the two factions of this frame (equal values for discontent and at war); the watcher listed them as duplicate lines. They are harmless, and a parser must not remove repeated lines inside a frame by comparing text.
- Only type codes 1, 3 and 5 have been produced; 2 (liberty), 4 (populist), 6 (nomadic) and 7 (other) have no sample.

### Frame sizes (session 2, "CURIA text" = the payload text; "in the file" includes the engine prefixes)

| Frame | Lines | CURIA text | In the file |
|---|---|---|---|
| Realm 3 (Ironman), Advisor | 7 | 231 B | 1085 B |
| Realm 3 (Ironman), probes | 51 | 1794 B | 8162 B |
| Realm 1, probes (the claims check; snapshots 71 and 72) | 64 | 2636 B | 10604 B |
| Realm 1, probes with the claimant faction (path B) | 77 | 3337 B | 13008 B |
| Realm 4, probes with a peasant and an independence faction | 81 | 3050 B | 13320 B |

No Advisor frame was recorded in realm 1 or realm 4 this session. The probe section was extended for this session, so these probe frames are not comparable with the 44-line probe frames of the re-check. The engine prefix was 119 to 131 bytes; the longest CURIA line was 321 B. All byte-budget windows were shorter than 10 minutes. T3 and T5 for the session are in their own sections above.

## Answered question: which claims does the export list?

**Question (asked after the re-check).** In realm 2 the owner saw **no claims in the UI**, but the export has one CLAIM line (a kingdom-tier title, `claim_tier` 4) and `ruler_claim_count` 1; the heir has 3. What was documented and what was not:

- **Documented (game files, the dump `triggers.log`):** `any_claim` iterates the claims a character holds, with parameters `explicit` = yes/no/all and `pressed` = yes/no/all. **The defaults are not stated.**
- **Concept text (installed localization, paraphrased):** children of a ruler have an implicit claim on the titles their parents hold; implicit claims turn into pressed claims if they are not inherited; unpressed claims pass to children.
- **The theory then open.** The export might include implicit or unpressed claims that the UI list does not show.

**Answer for the realms tested (session 2, "Session 2", claims).** In realm 3 the ruler's one claim is an **explicit, unpressed (weak) claim** and the character window lists it as an unpressed claim; in realm 1 the ruler's one claim is **explicit and pressed (strong)** and the window lists it as pressed. The implicit-claim theory is therefore **disproved for these two rulers**: the CLAIM line matches the UI list in both. Realm 3 has the same counts as realm 2 (ruler 1, heir 3), so the claim the owner did not see in realm 2 was most likely of the same kind, an unpressed claim the UI does list; that is an inference from equal counts, not a check of realm 2. An unfiltered `ordered_claim` behaved as `explicit` = all, `pressed` = all in every case measured. The export's CLAIM section (an unfiltered `ordered_claim`) therefore covers explicit pressed and unpressed claims appears to cover implicit ones too (by the filter default, inferred from equal counts in four cases); no ruler with an implicit claim was seen, so the CLAIM section's behaviour for implicit claims is not observed. For the two rulers seen it matches what the UI shows.

**Still unknown.** A ruler with an implicit claim (neither ruler had a living titled parent); the meaning of `pressed` for implicit claims beyond the heir counts above; per-claim kinds and UI list for an heir; an explicit pressed claim on an heir; the number the iterator returns for a ruler with many claims. The outcome for the wire-format checkpoint (which claim kinds the CLAIM lines should list, and whether implicit ones are a count) is **not decided here**.

**History.** `ordered_claim` ran in 91 exports through the re-check (71 + 17 + 3) and, with `check_range_bounds = no`, wrote no error in the re-check or in session 2.

## Where each topic is answered

This is a topic index, built by the maintainer from the owner's notes, the re-check and session 2; it does not reproduce the owner's own numbering. Only the one-vassal realm is attested as the re-check's finding 1.

| Topic | Where answered |
|---|---|
| Debug mode is a launch button, not a toggle | T10 |
| Console only opens in the debug launch | T10 |
| Achievements "Available", then "Unavailable" in the debug launch | T10, Achievements |
| `debug.log` and `error.log` truncated at launch | T5 |
| No LOAD line after loading a save; snapshot number persists | T5, T4 |
| Launcher rewrote `curia.mod` (final newline removed) | T11 row A |
| Buttons not clickable with the pause menu open | T7 |
| after_unpause shows 16 frames against a tally of 12 | T7 (cause unresolved) |
| The owner clicked faster than the sheet asked | T7 |
| The game keeps running while another app is in front | T7, proposal 4 |
| Income shows 8 for a HUD value of 7.8 | T7 (the `\|0` format rounds) |
| Vassal counts 11, 9 and 17 against 10 exported | Vassal counts |
| An error entry per export (claims error) | The claims error and the PR #11 fix |
| Error block inside every frame, 6 non-Curia lines | T4 (interleaving), the claims error |
| One CLAIM line while the UI shows no claims | Answered question: which claims (an unpressed claim the UI lists; inference for realm 2), Session 2 (claims) |
| Re-check finding 1: realm with one vassal | The claims error (no error with the fix: 1 VASSAL, 1 CLAIM; the vassal error itself was never observed) |
| Re-check: claims question | Answered question: which claims, Session 2 (claims) |
| Re-check: one late frame in latency run B | T3 |
| Re-check: 7 Advisor frames against a tally of 6 | T7 (cause not established) |
| Re-check: M3 status | Verdict table; remaining in-game items are under "Deferred or still untested" |
| Buttons in an Ironman game; achievements stay "Available" | Session 2 (Ironman), T7, Achievements |
| Claim counts per `explicit` / `pressed` pair; strong or weak; implicit claims count as pressed | Session 2 (claims) |
| Faction forms (type, power, threshold, discontent, members, leader, target) against the Factions window | Session 2 (factions), Value forms |
| Faction name carries link markup with control bytes | Session 2 (factions), T4, Inputs from session 2 |
| Power one apart (window against probe, and `_df` against `_sv`); `can_press_demands` and `dangerous` meanings | Session 2 (factions); not established |
| Two `LOAD` lines 40 s apart in the Ironman watch | T5, Session 2 (Ironman); cause not established |
| Session 2 latency (347 samples), frame sizes, error.log | T3, Session 2 (frame sizes), verdict criterion 6 |

## Deferred or still untested

Nothing here blocks starting Phase 2; items that bear on the spec are marked "for the wire-format checkpoint". Session 2 ran the faction-scope forms, the claims filters and the Ironman check, which are removed from this list; what they left open is below.

- **Faction forms still untested** (for the wire-format checkpoint): the `special_character` link and its data functions (the claimant) never ran; no discontent above 0 was ever seen; type codes 2, 4, 6 and 7 have no sample; the meaning of `can_press_demands` and `dangerous` is not established; why the power is one lower in the Factions window than in the probe, and why the two power forms differ by one for the independence faction, is not established.
- **Claims still unknown** (for the wire-format checkpoint): a ruler with an implicit claim; an explicit pressed claim on an heir; per-claim kinds and the UI list for an heir; how many claims the iterator returns for a ruler with many.
- **Button in other states:** map view variants, event windows during clicks; **observer mode** and **loading screens** (deferred by the owner, 2026-10-10, recorded open); repeated clicks in an Ironman game (one Advisor and one probes click only); the pause-menu click-then-Esc fallback (not run; the frame-span evidence shows only that an export is one burst, so whether a quick click-then-Esc registers is unmeasured).
- **Windowed** display mode: deferred to the M4 testing.
- **Text edge cases:** quotes, braces or newlines inside exported text; lines beyond 8,000 characters; whether a name can contain `|` (the faction name markup holds `;`, `,`, `:` and control bytes).
- **T9** clipboard comparison not run; **A15** file-change notifications not tested (deferred by the owner, 2026-10-10).
- **Latency:** sub-second resolution (needs a finer clock than whole-second engine stamps); the pause-menu T3 state (buttons not clickable there; deferred by the owner, 2026-10-10); fast speed in the re-check and session 2 (no per-state tally in session 2).
- **T8 per hour** of normal play (M4 criterion 8), the **17MB cap** (C3), **T11** beyond the default install (M7 criterion 10).
- **Other platforms and setups:** Windows and Linux log behaviour (tester checklist only), an Intel Mac, other macOS versions, other game versions, single-screen operation beyond the re-check and session 2.
- **Why the one extra Advisor frame in the re-check and the 16 against 12 after_unpause frames** appeared: unresolved. Also unresolved: why the Ironman watch holds two `LOAD` lines 40 s apart.

## Proposals for the wire-format checkpoint (not decisions)

Each item below is a **proposal for the wire-format checkpoint**, to be decided with the owner; none is adopted.

1. Proposal for the wire-format checkpoint: add a total-vassal-count field (the iterator form works) so a capped list is not mistaken for the whole realm.
2. Proposal for the wire-format checkpoint: cap wars, or decide not to, since they are uncapped at 3 lines each; add per-claim flags (the claims question is settled for two rulers by session 2's probes: `claim_detail` printed the four kind flags for the ruler, and the filter counts exist for ruler and heir; see "Session 2", claims).
3. Proposal for the wire-format checkpoint: reload handling. The app treats the newest complete frame as current; a falling or repeated snapshot number, or an earlier BEGIN date, signals a reload, because no LOAD line is written on load.
4. Proposal for the wire-format checkpoint: a "pause the game first" hint in the overlay, because the game keeps running while the player types.
5. Proposal for the wire-format checkpoint: the parser tolerates non-Curia lines inside a frame, BOM-less UTF-8 names, truncation at launch on the same inode, and frames of unknown size.
6. Proposal for the wire-format checkpoint: one more short in-game check before the freeze, with claim counts for `explicit` yes/no/all by `pressed` yes/no/all. **Done in session 2 (2026-10-11)**: the nine pairs for the ruler and the heir of realms 3 and 1 are in "Session 2", claims; what stays unknown is under "Answered question: which claims does the export list?".

## Inputs from session 2 for the wire-format checkpoint (facts, not decisions)

Facts from session 2 that bear on the wire-format spec; each is a datum, none is a decision, and the spec itself goes to the owner's checkpoint as a separate draft. The owner's own notes of 2026-10-11 for the spec (wanted from the spec, not decided): faction names carry markup, so prefer the type code, leader and claimant through the no-tooltip forms over the localized name, or strip it in the app, and make sure markup can never inject `|`; recommend one handling for implicit claims (listed in CLAIM lines, or only a count); one form per field; a field for the faction's claimant ("who might rebel and for whom"); keep the mod installed until the spec is approved.

1. **Markup in faction names.** The faction name carries link markup with control bytes (0x15) in the `GetNameNoTooltip` form, with `;`, `,` and `:` inside; no `|` in the one sample; the line was 321 B and flagged by the watcher. Leader and target names, the type code and the numeric fields were clean.
2. **Two forms of one value.** Power: the data-function and script-value forms were equal in two factions (33 and 33, 170 and 170) and one apart in the third (30 and 29); the claimant faction's value in the Factions window was 32 against 33 in both forms. Discontent: both forms printed zero in every faction seen (`0.00` and `0`, different formats); whether they agree at a nonzero value is untested.
3. **Discontent was 0 in every faction seen**, in both forms; whether the field carries information is untested.
4. **The claimant.** Recoverable today from the character id inside the name markup; the dumps list `special_character` forms (link, trigger and data functions) that have not run in the game.
5. **Claims.** An unfiltered `ordered_claim` behaves as `explicit` = all, `pressed` = all. Explicit pressed = strong and explicit unpressed = weak, as in the trigger names; an heir's implicit claims count as pressed. A ruler's claim list matched the UI for both rulers checked. The heir's claims are known by count only.
6. **Repeatable lines.** `LOAD` can repeat (two in one Ironman watch, 40 s apart, cause not established); identical per-faction PROBE lines can repeat inside one frame, so a parser must not remove repeated lines by comparing text.
7. **Empty and zero values exist.** A peasant faction had no leader (`none`), 0 members and a threshold of 0; its power was 170, above 100. `can_press_demands` was 1 for a faction below its threshold and 0 for another below its threshold, so the field is not derivable from power and threshold.
8. **Sizes.** Probe frames with factions were 77 and 81 lines (3337 and 3050 B of CURIA text); the longest line was 321 B, far below 8,000 characters; names with non-ASCII letters passed through unchanged.
