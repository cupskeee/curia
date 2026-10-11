<!-- SPDX-License-Identifier: MIT -->
# M3 income re-check: the FIN income figure against the top bar (owner, macOS, about 8 minutes once the game is loaded, about 12 with the install and the game start)

Why: the pre-freeze check of 2026-10-11 passed on every case except one, the `FIN` income. Mod 0.2.0 exported the income **before expenses**: for the king the tooltip showed income +9.2, expenses -1.3 and a total of +7.8, the top bar showed +7.8, and the log held 92 tenths (the tooltip's income line, 9.2). For the duke of the second game the top bar showed +4.9 and the log held 60 tenths (no tooltip lines were written down). The tenths themselves printed correctly; the quantity was the wrong one. Mod 0.2.1 exports the **net monthly balance** (income minus expenses), rounded once at the end, which is what the top bar shows (the tooltip's 9.2 and -1.3 are themselves rounded, so they need not add up to the displayed total exactly).

One gap is left open: on 19 May the tooltip showed income +9.2 while the three frames of that day held 90 (9.0); only the later frames (9 Aug, 3 Oct) held 92. The cause is not known. One candidate is that the tooltip, a window that is only refreshed while the game runs, was ahead of or behind the log at that moment, as the Vassals window was for vassal strength (it caught up a few seconds after unpausing); another is a real difference between the two figures. The **second reading in this sheet is required** because it separates them.

`docs/wire-format.md` is frozen only after this check passes.

Rules as in `m3-prefreeze-checklist.md`: a throwaway game, **plain Play button, no console, no debug mode**; words and numbers only in your notes, never game text; anything that differs from this page is the result, write it down. Also stop and write the exact text on a crash, a popup after a click, a permission prompt (before answering it) or an achievements line that changes in a plain Play. **Pause (Space) before reading the screen and before each click**, so the screen and the log show the same moment. One Terminal window, reached with Cmd+Tab.

## Before (CK3 and the launcher closed)
1. Terminal. `PATH_TO_CHECKOUT` is the folder with the mod 0.2.1 (the maintainer tells you which one; after the pull request is merged it is your own checkout after `git pull --ff-only`).
   ```sh
   cd PATH_TO_CHECKOUT
   git rev-parse --short HEAD
   tools/m3/install_mod.sh
   source tools/m3/session_env.sh
   grep -c 'version="0.2.1"' "$MODDIR/curia/descriptor.mod"
   grep -c curia_probe_ruler_net_tenths "$MODDIR/curia/localization/english/curia_l_english.yml"
   ```
   Both counts must be **1** (an older mod gives 0: stop). *Write:* the commit hash.
2. Launcher: playset `curia-m3` active with Curia on, plain **Play**. At the main menu run `errcount`. *Write:* the value (expected 0). **Not 0 is a finding**: run `errlines`, write the lines, stop. The new probe reads `monthly_character_balance`; the game's files use that name once, inside a scale block and never as the value of a named script value, so whether it loads in the form used here is unknown until this run, and an error at game start would name it.

## The check: two readings
**Reading 1** (right after the load, nothing unpaused yet; this is the situation of 19 May) and **Reading 2** (after the game has run for a few seconds). Both are required. Each reading is a pair: the screen and the log of the same paused moment.

3. Load `curia_m3_throwaway` and pause. Hover over the **gold or income figure in the top bar** (the one whose tooltip lists income, expenses and total) and *write all three lines of the tooltip with every digit*: income, expenses and total. Also *write* the figure the top bar itself shows. The top bar figure and the tooltip's total should agree; write it if they do not.
4. Click **Advisor once**. Cmd+Tab to the Terminal and run:
   ```sh
   python3 tools/m3/curia1_check.py "$LOGS/debug.log" --last 1
   ```
   *Write* the line `Income` of the table (tenths and gold). **Pass:** the log's tenths equal the tooltip's **total** times 10 (78 for +7.8), and the verdict is `Complete`. A difference of one tenth is not a pass or a fail by itself: write both figures.
5. Cmd+Tab back to CK3 (still paused) and click **Curia probes once** (the other button). Wait about 10 seconds, Cmd+Tab to the Terminal and run:
   ```sh
   grep -a -o 'CURIA1|[0-9]*|PROBE|ruler_[a-z_]*tenths|[-0-9]*' "$LOGS/debug.log" | tail -4
   ```
   It should print four lines of the last probe click, each with its name and a value in tenths: `ruler_income_gross_tenths` (the income before expenses), `ruler_expenses_tenths`, `ruler_balance_tenths` (the game's own balance figure) and `ruler_net_tenths` (the exported net figure; a negative number starts with a hyphen). *Write the four names with their values.* Fewer than four lines, or other names, is a finding: write what you see. Expected: `ruler_net_tenths` equals `ruler_balance_tenths`, and the income before expenses minus the expenses equals the net figure to within one tenth. Also compare `ruler_income_gross_tenths` with the tooltip's **income** line times 10: this is the 19 May question asked directly. Write any other result.
6. `errcount`. *Write:* the value (a rise is a Curia error: `errlines`, write every line).
7. **Reading 2 (required).** In CK3, unpause for about 10 real seconds (a few game days), pause again, and repeat steps 3, 4 and 5 (hover and write the tooltip again, **Advisor once**, the check command, **Curia probes once**, the grep). *Write* both readings side by side.

**How to read the two readings.** Compare, for each reading, the tooltip total with the log's `Income`, and the tooltip's income line with the probe's gross figure:
- Reading 1 differs and Reading 2 matches: the screen was behind the log at the first reading, as the Vassals window was for vassal strength. That explains 90 against 9.2; the log holds the current value, and a comparison should be made after a few seconds of running.
- Both readings differ by the same amount: not a lag. The quantity or the rounding is still wrong; the four probe values say which figure matches the tooltip total.
- Both readings match: the net figure is right. The 19 May difference is then not reproduced and stays unexplained; write that, and do not invent a cause.
- Reading 2 differs and Reading 1 matches: write both and say nothing more; this is a new finding.

## Send back
Quit CK3 cleanly, then in the same Terminal window:
```sh
cp "$LOGS/debug.log" "$RESULTS/debug-income.log"
cp "$LOGS/error.log" "$RESULTS/error-income.log"
python3 tools/m3/curia1_check.py "$RESULTS/debug-income.log" > "$RESULTS/check-income.txt"
```
Send your notes: for both readings the tooltip's three lines, the top bar figure, the checker's `Income` line and the four probe names and values, and both `errcount` values.

## Result table (fill in)
| # | Check | Tooltip / screen | Log / checker | Result |
|---|---|---|---|---|
| 1 | commit hash, version counts 1 and 1 | | | |
| 2 | `errcount` at the main menu (expected 0) | | | |
| 3 | Reading 1: tooltip total against the checker's tenths | | | |
| 4 | Reading 1: probe values (gross, expenses, balance, net) in tenths; gross against the tooltip's income line | | | |
| 5 | `errcount` after the clicks (expected 0) | | | |
| 6 | Reading 2 (after about 10 seconds of running): tooltip total against the checker's tenths, probe values, gross against the tooltip's income line | | | |
| 7 | which case of "How to read the two readings" applies | | | |
