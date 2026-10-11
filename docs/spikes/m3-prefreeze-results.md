<!-- SPDX-License-Identifier: MIT -->
# M3 pre-freeze check and income re-check: results (2026-10-11)

Numbers and words only; the owner's notes and the log files hold game text and stay private (`~/curia_m3_results/` on the owner's Mac). Screen observations (tooltips, windows, achievements, install counts) are the owner's notes; log values were read from the owner's logs. The sheets that were followed are `m3-prefreeze-checklist.md` (mod 0.2.0, about 30 minutes) and `m3-income-recheck.md` (mod 0.2.1, two readings). The format under test is `docs/wire-format.md`.

**Verdict.** Every must-test case passed except one field, the `FIN` income, which failed the first run (it was the income before expenses); case D covered one named war only. Mod 0.2.1 exports the net monthly balance, and the re-check showed the quantity is right. The exported net figure was 0.1 gold a month below the tooltip total in the first reading and 0.2 below in the second; the owner decided to freeze with the field documented as approximate to ±0.1 and, because the second reading was 0.2 below, widened the documented tolerance to ±0.2 the same day (see "Income" below). The format was frozen on this basis.

## Setup

| | Pre-freeze check | Income re-check |
|---|---|---|
| Date | 2026-10-11, single screen, macOS, CK3 Fullscreen | 2026-10-11, same Mac |
| Mod | 0.2.0 (commit `b8c3adb`) | 0.2.1 (commit `8fa29cb`) |
| Install counts | 1 and 2 as expected, no launcher warning | 1 and 1 as expected |
| `errcount` | baseline 0, final 0 | 0 at the menu, 0 after all clicks |
| Frames | 6, all Complete, none Damaged, none shortened | 4 (two Advisor, two probe), both Advisor frames Complete |
| Achievements | Available in both games | not read |

The first run had two games: a throwaway save of a king (Part 1) and a new game as a duke under an emperor (Part 2). The six frames are: Part 1, snapshot 71 (first click, 19 May 1069), 72 and 73 (the two fast clicks, same day), 74 (9 Aug 1069) and 75 (3 Oct 1071, the claimant faction); Part 2, snapshot 1 (15 Sep 1066). The war appears in all five Part 1 frames; there was no separate war click. Three `LOAD` records were written in the whole log (mod version `0.2.0` each time); the number "15619" in the notes was a line number.

## Results per case

| Case | Result | Notes |
|---|---|---|
| H date | **pass** | Year, month and day matched the top bar in Part 1; month and day are 1-based. Part 2's date was taken from the frame |
| H `LOAD` token | **pass** | `0.2.0` as one token, written at a new game (three in the first log). `0.2.1` was seen in `BEGIN` only: the re-check used a saved game, which writes no `LOAD` |
| E gold | **pass** | 650 and 156 matched the top bar. No gold of 1,000 or more was seen |
| E vassals window | **pass** | The window lists 11 vassals; the frame says `VASSALS` total 11, listed 10 (the cap), and the 10th and 11th vassals tied at strength 83 on screen. Part 2: 7 and 7 |
| E vassal gold, opinion, strength | **pass**, strength with a note | Gold is in each vassal's character window (the vassal tax shown elsewhere is a different number); opinion and strength are in the Vassals window. While paused the log's strength was 5 to 10 percent above the window for every vassal, and the window caught up a few seconds after unpausing: the log held the current value, so it is not a bug in the export |
| E negative numbers | **pass** (closed earlier from existing data) | Part 2 added negative opinions down to -72, all printed as an ASCII hyphen-minus |
| E income | **failed, fixed, re-checked** | See "Income" |
| A claimant faction | **pass** | One faction against the ruler, type claimant, power 33 (the window showed 33), 2 members, threshold shown in the window; leader is not labelled in the window but appears in the member list; `CLAIMANT` and `TITLE` (the title the faction wants) matched the window. **`TITLE` works and stays** (owner's condition for Q4). `ordered_targeting_faction` with `max`, `check_range_bounds = no` and `order_by` ran without an error (with one faction only; the cap with more than five factions was not exercised) |
| B own faction | **pass** | Duke's independence faction: `role` `member`, leader the duke, target the emperor, power 24, threshold 80, members 1; `CLAIMANT` and `TITLE` written as `none` for a type that is not claimant, as designed |
| D war | **pass** | One war, the ruler the attacker, name clean (no odd characters). It was a named historic war; a generated claim or casus-belli war was not tested |
| I two fast clicks | **pass** | Two clicks, two frames, both Complete, consecutive snapshot numbers |
| Heir and claims | **pass** | Heir name and opinion, and the explicit claim shown as pressed (`strong`), matched the character window |
| D15 | **pass** | No field fails: strength and opinion are in the Vassals window, vassal gold in the vassal's character window, the faction threshold in the Factions window, the leader in the member list |

## Income

**First run (mod 0.2.0).** `FIN` income was `monthly_character_income` times 10, printed with `|0`. For the king the tooltip over the top bar showed income +9.2, expenses -1.3 and a total of +7.8, the top bar showed +7.8, and the log held 90 tenths on 19 May (the three frames of that day, when the tooltip showed income +9.2) and 92 on 9 Aug and 3 Oct (the tooltip was not compared on those dates). For the duke the top bar showed +4.9 and the log held 60. The tenths printed correctly; the quantity was the income before expenses. The note in the session 1 results ("HUD +7.8, the probe prints 8") was a coincidence of the two figures, not evidence for the whole-gold form.

**Fix (mod 0.2.1, PR #22).** `FIN` income is now income minus expenses, times 10, with `round = yes`, in the field that keeps the name `income`. Four probe lines print the parts.

**Re-check.** Two readings on the throwaway save, the first at the 19 May state right after loading and the second after about 10 real seconds of running (29 May):

| | Reading 1 (19 May 1069) | Reading 2 (29 May 1069) |
|---|---|---|
| Tooltip income / expenses / total | 9.2 / 1.3 / 7.8 | 9.2 / 1.3 / 7.9 |
| `FIN` income in the log | 77 (7.7) | 77 (7.7) |
| Probes: gross / expenses / balance / net, in tenths | 91 / 14 / 77 / 77 | 91 / 14 / 77 / 77 |

Reading it:
- The quantity is now right: the exported net figure equals the game's own `monthly_character_balance` value (77; the game's balance value, used in a script value for the first time, raised no `error.log` entry) and 91 - 14 = 77.
- Gross income and expenses are each 0.1 off the tooltip lines (9.1 against 9.2, 1.4 against 1.3), in the same direction in both readings. The net figure is 0.1 below the tooltip total in Reading 1 (7.7 against 7.8) and 0.2 below in Reading 2 (7.7 against 7.9). The tooltip's own lines do not add up to its total in Reading 1 (9.2 - 1.3 = 7.9 against 7.8), so the tooltip shows rounded parts of hidden digits.
- It is not a lag between the screen and the log: Reading 1 was taken right after loading with no game time passing between the tooltip and the click, and the gross and expenses figures are already 0.1 off there. The log also did not change between the readings while the tooltip's total moved from 7.8 to 7.9, which a cached or coarse script-side value would equally produce (see the hypothesis below). The 19 May difference of the first run (90 in the log, 9.2 in the tooltip) is therefore a difference of values, not a stale screen.
- Rounding: the 19 May state in the re-check is the same as in the first run (gold 650 and all ten vassals' gold, opinion and strength identical in the first frame of both logs). With the same state the old form printed 90 for the gross income and the new form, with explicit rounding, printed 91, one data point showing that `|0` on a script value does not round to the nearest tenth (consistent with truncation). The offset to the tooltip is not explained by truncation or flooring in the mod: the expenses are *higher* in the log than in the tooltip, and the new form rounds to the nearest. This assumes the tooltip rounds to the nearest tenth, which was not verified. The cause is not established.
- **Hypothesis for M4, not tested:** the script-side income and expenses may be values the game updates at its monthly tick (the log stayed at 91/14/77 over ten days of the same month while the tooltip's total moved), whereas the tooltip is computed live. A test: unpause across a month boundary and see whether the log value changes only then.

**Decision (owner, 2026-10-11).** Freeze now. `FIN` income is the net monthly balance in tenths of a gold piece, approximate to ±0.1 at first and ±0.2 after the owner's later correction (the second reading was 0.2 below the tooltip total), like power ±1. A tenth of a gold a month changes no advice, and the field's meaning does not change if the rounding is improved later. No further PR for the rounding.

## The app's parser on the real logs

The app's headless reader (`curia --watch-log <file> --from-start`, built from the merged code, no window) was run over the owner's two real logs after the fact. The first run's log gave 6 Complete frames and the re-check's log 4, with no Damaged event and no diagnostic, and the values equal the Python checker's (for example the claimant faction with its claimant and title in the first run, the duke's own faction with no claimant and no title, `income_tenths` 90 and later 77). It is the first time the C++ parser read frames written by the game itself; the live latency of the app's own watcher against a running game has not been measured (the Python log watcher measured it in M3).

## Not tested (unchanged accepted gaps) and new notes

The accepted gaps of `wire-format.md` section 9 stay gaps: the peasant and populist `CLAIMANT` behaviour (case C), more than 10 vassals through the `listed` clamp beyond the one-over case seen, gold or strength of 10,000 or more (case E rest), a ruler with implicit claims or more than 10 claims (F), more than five factions (G), an heir whose liege is not the ruler (H rest), error entries quoting the marker (J), and the `none` forms of `HEIR` and `TARGET`. New or narrowed by this run:
- The vassal cap ran: 11 vassals, 10 listed, `total` 11.
- A generated claim or casus-belli war name was not seen (case D covered a named historic war).
- The `TITLE` form was seen with a claimant faction only; its value for other faction types is `none` by design.
- The cause of the 0.1 income offset to the tooltip is open.
- The watcher detects a truncation of the log only when the file is shorter than its read position at a poll (250 ms); a truncation followed by growth past the old position within one poll would look like an append. At game launch the log is truncated and grows slowly, and nothing was lost in this run; it is a known limit of the polling watcher (`wire-format.md` section 1).
