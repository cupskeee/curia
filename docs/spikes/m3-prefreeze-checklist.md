<!-- SPDX-License-Identifier: MIT -->
# M3 pre-freeze check: the CURIA1 format against the game (owner, macOS, about 30 minutes)

Why: `docs/wire-format.md` section 9 lists the in-game checks that must pass before the data format is declared frozen. The mod (version 0.2.0) now writes that format, and `tools/m3/curia1_check.py` reads the game's log and tells you, frame by frame, whether the records are valid and what to compare with the game's screen. This sheet is the **must-test** part only: cases A, B, D and I, plus the income part of E and the date part of H. The other cases are **accepted gaps**: they are not tested and are recorded as gaps (last section).

Rules as in `m3-session2-checklist.md`: throwaway games only, **plain Play button, no console, no debug mode** in everything below (the console switches achievements off for that game); words and numbers only in your notes, never game text (the files you send back do hold game text and stay private); anything that differs from this page is the result, write it down. One Terminal window, reached with Cmd+Tab (on a single screen it cannot stay in view next to CK3). **Pause the game (Space) before reading the screen and before every click**, so the numbers on screen and in the log are the same moment.

Words used: a **frame** is one export, started by one click on the **Advisor** button (the other button, Curia probes, is not used here). The **checker** is `curia1_check.py`; it prints a **verdict** per frame (`Complete`, or `Damaged` with a reason), a table of the realm, and a block **COMPARE WITH THE GAME UI** that names the numbers to compare with the screen. A Damaged frame is a result, not a reason to stop: copy the log (End) and go on. **D15** is the rule that Curia exports only what the game's own screen shows the player; a field the screen does not show **fails D15** and is removed before the freeze.

**Time box.** About 30 minutes of play once the game is running when things go as expected; the budgets below add up to 35 if every cap is used. The only open-ended step is waiting for a claimant faction; it has its own cap (8 real minutes). When time runs short, drop in this order: step 13 and any second war, then shorten the faction wait. Write what was not done and go on to the End: it becomes a gap. Write the real clock time at the start of each part (the session 2 sheet lacked it).

| Part | What | Budget |
|---|---|---|
| Before | install the changed mod, check it | 3 min |
| 1 | throwaway king: E (gold, income, Vassals window), H date, I, D, then A | 17 min, faction wait included |
| 2 | duke new game: H (LOAD line, date), B, a second look at E | 12 min, new-game setup included |
| End | copies, the final run, send back | 3 min |

**Stop rules.** `errcount` counts the `error.log` lines that mention the mod since the game was launched. Write its value at each check and compare it with the previous one: only a rise is a new Curia error. **The pass criterion is a final `errcount` of 0, or every counted line explained** (`wire-format.md` section 9): a mod error that appears at game launch (for example a wrongly written iterator in the new code) is already in the baseline, so a nonzero baseline is a finding too (step 3). Run it after the **first Advisor click** of each part. On a rise: `errlines`, stop clicking in that game, write every line (cut to 260 characters, kept local); the one exception is step 8, which has its own rule. Also stop and write the exact text on a crash, a popup after a click, a permission prompt (before answering it), or an achievements line that changes in a plain Play. An empty field or a 0 is a result, not a stop.

## Before (CK3 and the launcher closed)
1. Terminal. `PATH_TO_CHECKOUT` stands for the folder of the checkout that has the branch with the changed mod checked out (`feat/mod-curia1-export`; the maintainer tells you which folder). **Your own checkout is not touched**: `install_mod.sh` works from whichever checkout it lives in. After the pull request is merged, your own checkout is enough: `git pull --ff-only` there, then the same commands.
   ```sh
   cd PATH_TO_CHECKOUT
   git branch --show-current
   git rev-parse --short HEAD
   tools/m3/install_mod.sh
   source tools/m3/session_env.sh
   grep -c 'version="0.2.0"' "$MODDIR/curia/descriptor.mod"
   grep -c curia_line_faction_title "$MODDIR/curia/localization/english/curia_l_english.yml"
   ```
   The two counts must be **1** and **2** (an older mod gives 0 for the second: stop). If the branch is not in any folder yet, the maintainer can make one for you with `git worktree add --detach <new folder> feat/mod-curia1-export` run in your repository; it does not change your checkout. (`--detach` is needed while another folder still has the branch checked out; in that folder `git branch --show-current` prints an empty line, which is fine: the commit hash from `git rev-parse --short HEAD` is what you write.) Use the same Terminal window for everything below (the `source` line defines `errcount`, `errlines`, `LOGS` and `RESULTS`).
2. Launcher: playset `curia-m3` active with Curia on. Start with the plain **Play** button. *Write:* launcher warnings, if any; CK3 version; the commit hash (and the branch name, if one was printed) from step 1.
3. At the main menu: `errcount`. *Write:* the value (the baseline). **If it is not 0**, run `errlines`, write every line and treat it as a finding before you play: the mod's new code has never run in the game, and a parse-time error shows up here, not at a click.

The log watcher (`logwatch.py`) is **not needed**: this is one launch of the game, `debug.log` is only emptied at launch, and the copy taken at the End holds every record. If you want the latency and size numbers again anyway, start `python3 tools/m3/logwatch.py --label prefreeze` in a second Terminal tab before step 2.

## Part 1: the throwaway king (about 17 min)
Write the clock time. The check command used below (after every group of clicks) is this one; `--last 1` prints the table of the newest frame, and the SUMMARY at the end always covers all frames since launch:
```sh
python3 tools/m3/curia1_check.py "$LOGS/debug.log" --last 1
```
4. Load `curia_m3_throwaway`, pause. Read the achievements indicator (pause menu, second icon by Game Rules) and *write* its text (expected: available; this is the Part 1 reading, the game is gone at the End). Read the screen and write it down **before** clicking: the date (day, month name, year), the gold, and the monthly income (hover over the gold in the top bar for the exact monthly income and write every digit the tooltip shows, decimals included). Open the Vassals window (case E, D15): *write* which columns it shows for a vassal (gold? military strength? opinion?) and the numbers it shows for the first few vassals, in the order the window lists them.
5. Click **Advisor once**. Cmd+Tab to the Terminal: `errcount`, then the check command. *Compare with the block COMPARE WITH THE GAME UI, line by line, and write for each:*
   - **H date:** day, month and year printed against the top bar. A month or day one off means the numbering base is wrong.
   - **E gold** against the top bar; **E income:** the log prints it in tenths (78 means 7.8) and the checker shows it as gold with one decimal; *write* the on-screen figure with all its digits and the checker's figure. **Criterion:** the log's tenths equal the on-screen figure rounded to the nearest tenth (7.84 gives 78; 7.85 may give 78 or 79 and is not a finding). Any other result, for example a truncated figure, is the finding: write both figures.
   - **E vassals:** the total against the Vassals window, then each listed vassal's gold, opinion and strength against the window. If the window does not show gold or strength, write: "gold (or strength) fails D15".
   - the heir (name, opinion), and the claims (the checker prints pressed or unpressed; the character window shows the same words).
   - the verdict (`Complete` expected), and any line under "records that failed the grammar".
6. **Case I, two clicks in a row.** Still paused, click **Advisor twice as fast as you can** (if the first try was slow, once more). Cmd+Tab, check command with `--last 2`. Expected: two frames, both Complete, consecutive numbers. *Write:* how many clicks, how many frames, the verdicts.
7. **Case D, a war** (at most 4 minutes). Open the realm's wars. *Write:* how many wars the king is in. If none, start one: the king has one claim, and a war over that claim is the cheapest; the game's declare-war screen shows what is possible. If no war can be started in 4 minutes, write "no war set up" (go to step 8; case D may be covered in Part 2). With a war: pause, click **Advisor once**, check command. Compare: `Wars` total against the game's list; the war's name; which side the king is on (the checker prints "the ruler is the ATTACKER" when the king declared it; "an ally" when neither listed id is the king's). *Write:* any text in a war name that looks like leftover markup or odd characters, and the line "text fields changed by the hygiene rules" in the SUMMARY (it names the record types, for example `WAR NAME x1`). A second war, if one exists (an ally's war counts), is welcome, not required.
8. **Case A, a faction against the king** (cap: 8 real minutes; write the clock times at the start and end). Open the game's Factions window. If a faction against the king is already listed, go straight to the click. If not, run at speed 5 and watch the window until a faction against the king appears (the claimant faction of session 2 appeared this way), then pause at once. **Case A counts fully only for a claimant faction** (it is the only kind with a `claimant` and a `title`). If the first faction is another kind (independence, peasant), click and compare it anyway, then keep waiting within the same cap for a claimant faction. If none appears within 8 real minutes, write "no faction seen in N minutes" (or "no claimant faction seen") and go to Part 2: case A, rows 17 and 18 become gaps and the result for row 18 is **TITLE untested**, which the maintainer decides on.
   **If `errcount` rises at this click:** run `errlines`, mark row 16 as **failed** (the `ordered_targeting_faction` form has never run in the game; a failure is an expected, recordable result), take no more Advisor clicks in this game, and continue with Part 2 as usual.
   With a faction: click **Advisor once**, check command. *Compare with the Factions window* (the block names each faction): how many factions against the king; for each its kind, power in percent (it can differ from the log by 1; write both), the member count, the leader, the target; **does the window show a threshold** (the log has one)? If not, write "threshold fails D15". For a **claimant** faction: the window shows the claimant and the title it wants; the log has `claimant` and `title` lines. *Write:* claimant and title in the log against the window (names are not needed, write "match" or "differ" and the title's tier word); if the log says `none` where the window shows a claimant or a title, write that; and the **TITLE verdict: works / does not work (remove) / untested**. Also compare the claims shown: the character window against the checker's pressed or unpressed. `errcount` again.

## Part 2: the duke new game (about 12 min)
Write the clock time.
9. Quit to the main menu (do not close CK3). Start a new game from scratch: the 1066 bookmark, **Ironman off**, a **duke with a king above**, ideally one who has vassals of his own and an heir; pick the character in the start screen (this takes a few minutes, which is why Part 2 has 12). When the game has loaded, pause, read the achievements indicator as in step 4 and *write* its text (the Part 2 reading), and before any click run the check command: the SUMMARY line **LOAD records (snap 0)** must now list one more entry with `version 0.2.0` (a second LOAD line is possible and harmless; write how many). *Write:* the number of LOAD lines and their version words (**H, the LOAD token form**).
10. Read the screen and write it down: date, gold, income (exact, with decimal), whether the Vassals window lists any vassal, the heir (or none). Click **Advisor once**, `errcount`, the check command. Compare as in step 5 (**H date** again, on a second date: it is the new game's start date; **E gold and income** again; the Vassals window). *Write:* all differences, and whether `HEIR` printed `none` or an heir (a ruler without an heir is a free test of the `none` form).
11. **Case B, a faction the ruler belongs to.** Open the Factions window and create an independence faction (as in session 2; write which kind it is and the requirements it showed). Pause, click **Advisor once**, check command. Expected: `ruler is a member of one: yes`, one faction with role `member` and type `independence`. Compare with the window: kind, power, members, leader (the duke himself), target (his liege). The lines `claimant` and `title` of an independence faction must read `none`. *Write:* what the checker printed for `leader`, `claimant`, `title`, `target` and the verdict.
12. **Negative numbers** (case E): closed from existing data, nothing to do. The recorded logs of the earlier sessions hold 120 negative vassal opinions (lowest -55) printed as an ASCII hyphen-minus before the digits, in the same script-value form the new `VASSAL` record uses, and no non-ASCII minus (see `wire-format.md` section 9). The checker's SUMMARY line "negative integers seen: none (case E needs at least one)" is older wording: ignore it. If a negative `income` or `gold` happens to be on screen at a click, write the field and the printed form; do not look for one.
13. *Optional, the first thing dropped when time is short.* If the duke is in a war (his own or an ally's, check the war list) and Part 1 had no war, click and compare as in step 7. *Write:* what applied.

## End
14. Before the next launch of CK3, in the Terminal (the achievements readings were made in steps 4 and 9):
    ```sh
    errcount
    cp "$LOGS/debug.log" ~/curia_m3_results/debug-prefreeze.log
    cp "$LOGS/error.log" ~/curia_m3_results/error-prefreeze.log
    python3 tools/m3/curia1_check.py ~/curia_m3_results/debug-prefreeze.log > ~/curia_m3_results/check-prefreeze.txt
    tail -40 ~/curia_m3_results/check-prefreeze.txt
    ```
    A nonzero final `errcount`: `errlines` first; the criterion is 0 or every line explained (see the stop rules). *Write:* the final `errcount`, and the `frames:` and `RESULT:` lines of the SUMMARY.
15. **Send back, privately** (the files hold the game's own text; not committed or posted): `check-prefreeze.txt`, `debug-prefreeze.log`, `error-prefreeze.log`, and your notes with the filled table below.
16. **Cleanup** (CK3 and launcher closed): delete the throwaway saves if wanted. Keep the mod installed until the freeze is declared; remove it later with `tools/m3/install_mod.sh --uninstall`.

## Results table (fill in; pass, fail or gap, then the notes)
| # | Row | Pass / fail / gap | Notes (what the screen showed against the log) |
|---|---|---|---|
| 1 | Install check: counts 1 and 2, commit hash, CK3 version | | |
| 2 | `errcount`: baseline at the menu, after first click (part 1), after first click (part 2), end. Pass: end = 0 or every line explained; a nonzero baseline is a finding | | |
| 3 | Achievements text read in step 4 (part 1) and step 9 (part 2) (expected: available) | | |
| 4 | H date, throwaway king: screen against log (day, month, year) | | |
| 5 | H date, duke new game: screen against log | | |
| 6 | H `LOAD` line: how many, version word | | |
| 7 | I two quick clicks: clicks, frames, verdicts | | |
| 8 | E gold: screen against log | | |
| 9 | E income in tenths: every digit of the tooltip figure against the checker's figure, both games. Pass: log tenths = screen figure rounded to the nearest tenth | | |
| 10 | E a negative integer printed: **pass from existing data** (vassal opinion, ASCII hyphen-minus, 120 of 920 values, lowest -55, no `-0`, no non-ASCII minus). Optional here: a negative `income` or `gold`, field and printed form | pass (prefilled) | |
| 11 | E Vassals window against the log: total, listed, gold, opinion, strength | | |
| 12 | D15 Vassals window shows gold? shows strength? shows opinion? | | |
| 13 | D `WARS` total, war name, side (attacker, defender, ally) against the game | | |
| 14 | D war-name markup: "hygiene" count and record types | | |
| 15 | A factions against the king: count, kind, power, members, leader, target against the Factions window | | |
| 16 | A `ordered_targeting_faction` (capped, ordered listing): worked / failed (frame Complete, `against` equals the window's count) | | |
| 17 | A claimant and title of a claimant faction against the window | | |
| 18 | **TITLE: works / does not work, remove / untested** (untested if no claimant faction appeared) | | |
| 19 | A `CLAIM` lines: pressed or unpressed against the character window; explicit and implicit counts | | |
| 20 | D15 threshold: does the Factions window show a threshold? | | |
| 21 | B member faction: role, type, leader, target, `none` forms of claimant and title | | |
| 22 | **Fields that fail D15** (list every field the screen does not show) | | |
| 23 | Damaged frames (number, reasons) and grammar failures (number, reasons, line numbers) | | |
| 24 | `error.log` lines mentioning the mod at the end | | |
| 25 | Time: clock times of part 1, the faction wait, part 2; what was not done | | |

## Accepted gaps (not tested, recorded as gaps)
The owner accepted these unless they turn out to be easy to set up; they are listed in the results as untested, not as passed. What a failure would look like is in `wire-format.md` section 9.
- **C** a peasant or populist faction (which faction types return a character for `claimant`; the `none` form of `leader`).
- **E, rest** more than 10 vassals (the `listed` clamp); gold or strength of 10,000 or more (plain digits, no grouping separator or abbreviation). This is the highest-risk gap: a rich realm would fail visibly, every time. Existing logs narrow it: strength values of four digits (up to 4,725) printed as plain digits; nothing of 10,000 or more has been seen.
- **F** a ruler with implicit claims; more than 10 claims. **G** more than 5 factions against the ruler. **H, rest** an heir whose liege is not the ruler. **J** error-entry lines in `debug.log` after a mod error. **`HEIR` none and `TARGET` none** unless they came up by themselves (step 10 and step 11 say where to note it).
- Open assumptions stay open (wire-format section 9): other game languages, very long names, names with quotes, braces or newlines.

**Optional console route for the edge cases (10,000 or more gold, many vassals, many claims).** Only if you want to close them, and **only in a separate throwaway game that is never used for the cases above**: quit CK3, start the game with the one-shot launch button next to "Open game in Debug Mode" (Game Settings, Game tab; not a setting that stays on), start a new game, and use the game's console to give the character the gold, vassals or claims. Using the console switches achievements off for that game, which is why this game is separate and thrown away afterwards; the next launch must use the plain Play button. Debug mode must not change what is measured (number formatting, list caps), so write the exact line the checker prints for the large number. The exact console commands are not given here because they have not been checked against the game's files (Curia's rule is to take game script from the local dumps, not from memory): ask the maintainer for the lines before you try, or skip the route. Run the checker on that game's log the same way (`curia1_check.py` on the `debug.log` copy).
