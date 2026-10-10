# Deferred measurements: T8 per hour and T11 launcher variants (not part of M3)

> Note, 2026-10-10: the M3 sheets this file refers to are `m3-protocol.md` (reference) and `m3-quickrun-session1.md` (the sheet followed on 2026-10-06/07); the results are in `docs/spikes/m3-results.md`. This file itself was not run. Wording corrected afterwards: Debug mode is not a launcher setting that stays on or off. In the launcher, Game Settings, Game tab, next to the entry "Open game in Debug Mode", there is a one-shot **launch button**: only a game started with it is a debug launch, there is nothing to switch on or off afterwards, and a plain Play is never a debug launch. The display mode can also be set in the launcher's Game Settings. The console was reached only in the debug launch; one press of the console key in the plain launch of the session opened none. The pre-launch checks, stop rules, cleanup and table rows that treated debug mode as a persistent setting are reworded below; the commands are unchanged and the table rows keep their positions.

Status: **deferred, not run, not part of M3.** Owner decision of 2026-10-06: the T8 one-hour stretches move to **normal play after M4** (Curia installed, measured with `tools/m3/logwatch.py` or the app's own watcher once it counts bytes), and T11 (which launcher metadata the macOS launcher needs, and how `supported_version` values behave on the installed 1.20.x game) moves to **M7 packaging**, beside the Paradox Mods path decision. The maintainer's analysis, approved by the owner, is in `docs/milestones.md` (M3, M4, M7) and `docs/research-notes.md` §5.1 and §4.1.

This file keeps the procedures and recording tables that were prepared for the second M3 sitting and checked on paper only. Nothing here is a result. They were written for a different moment than the one they will now run in (M4 normal play for T8, M7 for T11): **re-check every command against the tools before use** (`tools/m3/session_env.sh`, `tools/m3/install_mod.sh`, `tools/m3/logwatch.py`, `tools/m3/frame_table.py` exist; their options may have changed) and adapt the steps: in normal play with Curia installed the watcher separates CURIA bytes from the game's own, so a normal-play window is expected to yield the baseline and the separate no-click stretch below may be dropped. Row A of the T11 table (both metadata files, `supported_version` `1.20.*`, default install) is **not** deferred: it is observed in the M3 session (`m3-protocol.md`, R7). The per-export bytes (T8) are also measured there (R6).

The rules of `m3-protocol.md` (two Terminal tabs, throwaway games only, words and numbers only, no game text in tables, stop rules, plain Play only) apply here unchanged. The C3 cap (17MB) stays deferred as before; its start procedure is kept in 3.6 below.

## Protocol parameters (T8 stretches)
Chosen values of the protocol, not predictions.

| Parameter | Value | Why |
|---|---|---|
| Hour stretch (T8) | 60 min, one Advisor click about every 5 real minutes (minutes 5, 10, ... 55) | per-hour growth in normal use |
| Baseline stretch (T8) | 15 min, **no clicks**, same game speed as the hour stretch | the game's own logging |
| Game speed in the T8 stretches | speed 3 (digit key 3), written down and kept the same in both stretches | a protocol choice |

## Preflight (CK3 and the launcher closed; record in R0)
1. Tab 1: `cd` as in 0.1 of `m3-protocol.md`. Tab 2: `cd ~/PycharmProjects/curia && source tools/m3/session_env.sh` again.
2. `python3 tools/m3/logwatch.py --selftest` (tab 2) ends with `SELFTEST_OK`. If the checkout changed since the M3 session, repeat 0.2 first.
3. **Mod installed?** `ls -l "$MODDIR" "$MODDIR/curia" "$MODDIR/curia/.metadata"` (tab 2) must show `curia.mod`, `curia/descriptor.mod` and `curia/.metadata/metadata.json`. If not, `tools/m3/install_mod.sh` (CK3 closed) and look again after the launcher is open (0.7).
4. Pre-launch check (0.7): playset `curia-m3` active with "Curia" enabled, and the game started with the **plain Play button** (never the debug launch button).
5. **Achievements baseline:** at the new-game setup screen of the first launch of the sitting the Achievements line must read "Available"; if "Not available", the stop rule applies. Display mode Fullscreen (confirm at the main menu).

## Part 3. T8: bytes per hour, baseline and the cap (originally launch 3)

Originally: launch 3 (plain Play), then a **new game** `curia_m3_t8_throwaway` with a ruler like in 2.3 of `m3-protocol.md` (so that exports are comparable with its Part 2), then pause and start the watchers below once the game is running. In the deferred form (normal play after M4) the game, the realm and the mod are whatever normal play uses.

**3.1 Bytes per export.** Measured in the M3 session (its R6, from the click protocol); not repeated here. The by-product growth figure from the M3 window (launch, new game, clicks) is also recorded there and is not representative of normal play.

**3.2 Bytes per hour, with clicks.** Keep the Mac and its display awake (tab 2; the command ends by itself after 90 minutes):
```sh
caffeinate -di -t 5400 &
```
In tab 1 start the run:
```sh
python3 tools/m3/logwatch.py --duration 3600 --report-every 60 --label hour_clicks
```
It attaches at the end of the current logs, prints a status line per minute with the file sizes and the growth, and **stops by itself after 3600 s**, printing and saving the summary. During the hour: play normally at **speed 3**, **one Advisor click about every 5 minutes** (a timer helps; the exact moment is not important). If an event pauses the game, deal with it and go on; write down roughly how many minutes the game spent paused. Note in tab 1, via `!` lines at the start and the end: the HUD date and speed (`!start 1066.10.1 speed 3`, `!end ...`). If a faction forms against the ruler during the hour, click **Curia probes** once while it exists and note it (the faction probes were never run).

**3.3 Baseline, no clicks.** Right after the hour (a gap of about a minute at most, the same game, **speed 3**, no Advisor or probes click at all), in tab 1:
```sh
python3 tools/m3/logwatch.py --duration 900 --report-every 60 --label baseline_no_clicks
```
Note the HUD date at the start and the end of this stretch as well.

**3.4 Read** (R6): section 7 of each summary gives, for `debug.log` and `error.log`: bytes excluding CURIA lines, CURIA lines, total, per minute and per hour (the tool prints a per-hour figure for any window of 10 minutes or more and "n/a" under 10, so both stretches get one). Write down the **raw totals of both stretches**, the real elapsed minutes, the in-game days that passed, and the per-hour figure **as printed**. The hour's total without its CURIA part, set against the baseline's rate, shows whether the game's own logging differs between the stretches; the CURIA part divided by the number of clicks is the per-export cost over a long run. The rates are totals over the measured window divided by its length, not forecasts.

**3.5 error.log check again** (tab 2): `errcount` (set `JSON=$(ls -t ~/curia_m3_results/watch-*.jsonl | head -1)` again before comparing with the watcher's `error_hit` count), then steps 4 and 5 of Part 5 of `m3-protocol.md` (R8 row "end of Part 3" below).

**3.6 The 17MB cap (C3): not part of the M3 session.** The claim (`research-notes.md` C3): CK3 may stop writing logs after about 17MB of **cumulative bytes written** by one game process, counted over `debug.log` and `error.log` together, not reset by truncating a file, reset only by restarting the game. A reproduction needs one very long play session with the watcher running; many exports cannot be produced in a loop without the console (each export is one manual click), so the cap cannot be forced quickly. It is **deferred, not failed**, until the per-hour rates of 3.4 are known. To start it later: start the watcher before CK3 (as in 2.1) and leave the game running with `python3 tools/m3/logwatch.py --report-every 300 --label long` in tab 1; the status line shows both file sizes and the bytes appended since the watcher started. The sign to look for is activity that stops producing lines while the game clearly runs and the sizes stop growing; then write down the totals at that moment and whether `error.log` stopped too.

## Part 4. T11: mod metadata and `supported_version`

**T11 procedure, one launch per row.** The mod must be installed with CK3 and the launcher **closed** (the installer refuses while `ck3` runs and the launcher may rewrite the folder). This part needs the 0.1 block and helper functions of `m3-protocol.md` in tab 2 (the preflight above covers it). Row A (default install) is observed in the M3 session (0.6, 0.7, 2.3 there). For each row B to G, in this order:

1. Quit CK3 **and** the launcher. Install the row's variant (tab 2; command in the table). Then `t11_snap <row>-before`.
2. Open the launcher. Look at the mod list of the playset `curia-m3`: is "Curia" listed, enabled or disabled, is there any warning or "outdated" mark (exact text), did the playset need the mod added again, did the launcher need a restart to see it. Then `t11_snap <row>-after` and `diff ~/curia_m3_results/t11-<row>-before.sha.txt ~/curia_m3_results/t11-<row>-after.sha.txt` (empty output = the launcher changed nothing; a difference = it changed or removed something: say what, with `ls -l "$MODDIR" "$MODDIR/curia"`).
3. Play (plain Play). Does the game start without a warning of its own about the mod (exact text if one appears)? At the main menu run `t11_check <row>-menu` (tab 2).
4. Start a **new game** (any bookmark, Ironman off, name `curia_m3_t11_<row>`; the game-start line is expected at a new game, whether a loaded save prints it is not known), wait about 15 s, run `t11_check <row>-game`. **Mod-loaded evidence = the `LOAD` count above is 1 after the new game** (the second evidence is the "other lines mentioning curia" count, a count of lines, not their text, compared with the `-menu` count; the M0 stub's load showed up as mount lines). If the launcher refused the mod or the game started without it, skip the new game and record that.
5. Quit CK3. Next row.

| Row | Install command (CK3 and launcher closed) | The question it answers |
|---|---|---|
| A | `tools/m3/install_mod.sh` (M3 session, 0.6 there; the observations are made in 0.7 and 2.3 there) | the default: `descriptor.mod` and `metadata.json` both present, `supported_version` `1.20.*` |
| B | `tools/m3/install_mod.sh --variant descriptor` | `descriptor.mod` only inside the folder, no `.metadata` folder, outer file present: listed, loaded? |
| C | `tools/m3/install_mod.sh --variant metadata` | `.metadata/metadata.json` only inside the folder, no `descriptor.mod`, outer file present: listed, loaded? |
| D | `tools/m3/install_mod.sh --supported-version '1.19.*'` | both files, `supported_version` 1.19.* on the installed 1.20.x game: outdated warning? loaded? |
| E | `tools/m3/install_mod.sh --supported-version '1.18.*'` | both files, `supported_version` 1.18.*: same questions, one more version back |
| **F (required)** | `tools/m3/install_mod.sh --variant metadata && rm "$MODDIR/curia.mod"` | the folder with `metadata.json` only and **no outer `curia.mod`**: does the launcher find a mod that has no outer file at all? |
| **G (required)** | `tools/m3/install_mod.sh --variant descriptor && rm "$MODDIR/curia.mod"` | the folder with `descriptor.mod` only and **no outer `curia.mod`**: the counterpart of F |

Reading the table: the installer writes the outer `curia.mod` (the game's mod-list entry: the repository descriptor plus `path=`) in **every** variant, so rows B and C vary only the files **inside** the folder and **do not isolate descriptor versus metadata**; F and G, the only rows without the outer file, do, and "which of `descriptor.mod` / `metadata.json` the launcher needs" is read from F against G (B and C support it). Quote `'1.19.*'` (the shell would try to expand the star). The row A value `1.20.*` is also what `--supported-version '1.20.*'` would write, so it is not a separate row. `metadata.json` was written from the launcher's own validation schema (the optional `game_id` is left out): row C is the first time the launcher reads it. After row G, `tools/m3/install_mod.sh` (default) leaves the mod ready for later, or go to the cleanup below. (T12, ck3-tiger, is not deferred; it is reported in `m3-protocol.md`.)

## Recording sheet (deferred cells)
Words and numbers, no game text.

**R0 cells**
| Item | Value |
|---|---|
| Sitting started with plain Play, not the debug launch button (yes/no) | |
| Achievements line at the setup screen of the first launch of the sitting (baseline) | |

**R6 T8 stretches** (the per-export bytes table stays in `m3-protocol.md`)
| Stretch | Elapsed min | In-game days | Clicks | `debug.log` total / CURIA part | `error.log` total | Per hour as printed |
|---|---|---|---|---|---|---|
| 60 min with clicks (speed 3) | | | | | | as printed |
| 15 min baseline (speed 3) | | | | | | as printed |
Cap (C3): **deferred** (state, reason in 3.6).

**R7 T11 rows B to G** (row A is in `m3-protocol.md`)
| Row | Listed / enabled | Warning text | Playset needed re-adding / launcher restart | Folder changed by the launcher (`diff`) | Game's own warning | LOAD count after the new game | Other curia lines (count) | error.log `curia[_/]` count | Verdict (loaded / not loaded / refused) |
|---|---|---|---|---|---|---|---|---|---|
| B descriptor only (outer file present) | | | | | | | | | |
| C metadata only (outer file present) | | | | | | | | | |
| D both, 1.19.* | | | | | | | | | |
| E both, 1.18.* | | | | | | | | | |
| F metadata only, no outer file | | | | | | | | | |
| G descriptor only, no outer file | | | | | | | | | |

**R8 cells** (`errcount`; indicator read at the setup screen / pause menu / Game Rules)
| Moment | error.log `curia[_/]` count | Indicator: setup / pause menu / Game Rules |
|---|---|---|
| Start of Part 3 | | |
| End of Part 3 | | |

## What to send back
The cells and tables of this file, and the files of these runs in `~/curia_m3_results/` (they hold the game's own log text: privately to the maintainer, never committed or posted); anything written down verbatim under the stop rules. Same rules as in `m3-protocol.md`.

## Cleanup (after the results are in; CK3 and the launcher closed)
Tab 2: `tools/m3/install_mod.sh --uninstall`, then `ls -l "$MODDIR"` (the listing should show only the workshop `.mod` files again, the M0b state), unless the mod is meant to stay installed. In the launcher switch back to the usual playset (the debug launch is a button, not a setting that stays on), delete the `curia-m3` playset if it was used, and confirm the achievements indicator reads "Available" at a plain Play's new-game setup screen. Delete the throwaway games (`curia_m3_t8_throwaway`, `curia_m3_t11_*`) in Load Game. `caffeinate` ends by itself (`killall caffeinate` if it still runs). Keep `~/curia_m3_results/`.
