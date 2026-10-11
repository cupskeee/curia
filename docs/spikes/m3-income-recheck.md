<!-- SPDX-License-Identifier: MIT -->
# M3 income re-check: the FIN income figure against the top bar (owner, macOS, about 5 minutes once the game is loaded, about 10 with the install and the game start)

Why: the pre-freeze check of 2026-10-11 passed on every case except one, the `FIN` income. Mod 0.2.0 exported the income **before expenses**: for the king the tooltip showed income +9.2, expenses -1.3 and a total of +7.8, the top bar showed +7.8, and the log held 92 tenths (the tooltip's income line, 9.2). For the duke of the second game the top bar showed +4.9 and the log held 60 tenths (no tooltip lines were written down). The tenths themselves printed correctly; the quantity was the wrong one. Mod 0.2.1 exports the **net monthly balance** (income minus expenses), rounded once at the end, which is what the top bar shows (the tooltip's 9.2 and -1.3 are themselves rounded, so they need not add up to the displayed total exactly). `docs/wire-format.md` is frozen only after this check passes.

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

## The check
3. Load `curia_m3_throwaway` and pause. Hover over the **gold or income figure in the top bar** (the one whose tooltip lists income, expenses and total) and *write all three lines of the tooltip with every digit*: income, expenses and total. Also *write* the figure the top bar itself shows. The top bar figure and the tooltip's total should agree; write it if they do not.
4. Click **Advisor once**. Cmd+Tab to the Terminal and run:
   ```sh
   python3 tools/m3/curia1_check.py "$LOGS/debug.log" --last 1
   ```
   *Write* the line `Income` of the table (tenths and gold). **Pass:** the log's tenths equal the tooltip's **total** times 10 (78 for +7.8), and the verdict is `Complete`. A difference of one tenth is not a pass or a fail by itself: write both figures and go on to step 5.
5. Cmd+Tab back to CK3 (still paused) and click **Curia probes once** (the other button). Wait about 10 seconds, Cmd+Tab to the Terminal and run:
   ```sh
   grep -a -o 'CURIA1|[0-9]*|PROBE|ruler_[a-z_]*tenths|[-0-9]*' "$LOGS/debug.log" | tail -4
   ```
   It should print four lines of the last probe click, each with its name and a value in tenths: `ruler_income_gross_tenths` (the income before expenses), `ruler_expenses_tenths`, `ruler_balance_tenths` (the game's own balance figure) and `ruler_net_tenths` (the exported net figure; a negative number starts with a hyphen). *Write the four names with their values.* Fewer than four lines, or other names, is a finding: write what you see. Expected: `ruler_net_tenths` equals `ruler_balance_tenths`, and the income before expenses minus the expenses equals the net figure to within one tenth. Write any other result. This tells which quantity matches the tooltip's total if step 4 did not.
6. `errcount` again. *Write:* the value (a rise is a Curia error: `errlines`, write every line).
7. Optional, 1 minute: unpause for a few days, pause again and repeat steps 3 and 4. A second reading tells whether the figure follows the tooltip when it changes.

## Send back
Quit CK3 cleanly, then in the same Terminal window:
```sh
cp "$LOGS/debug.log" "$RESULTS/debug-income.log"
cp "$LOGS/error.log" "$RESULTS/error-income.log"
python3 tools/m3/curia1_check.py "$RESULTS/debug-income.log" > "$RESULTS/check-income.txt"
```
Send your notes: the tooltip's three lines, the top bar figure, the checker's `Income` line, the four probe names and values, both `errcount` values.

## Result table (fill in)
| # | Check | Tooltip / screen | Log / checker | Result |
|---|---|---|---|---|
| 1 | commit hash, version counts 1 and 1 | | | |
| 2 | `errcount` at the main menu (expected 0) | | | |
| 3 | net income: tooltip total against the checker's tenths | | | |
| 4 | probe names and values (gross, expenses, balance, net) in tenths | | | |
| 5 | `errcount` after the clicks (expected 0) | | | |
| 6 | optional second reading | | | |
