# M3 session: quick run (owner, macOS)

The full sheet `m3-protocol.md` stays the reference; section numbers and tables (R0 to R8) in brackets point into it. This page is the commands in order and what to write down. Two Terminal tabs, **put this Terminal on the display that is not CK3's**: **tab 1 only runs the watcher** (labels and `!` notes), **tab 2 runs everything else**. Words and numbers only, never game text. Throwaway games only, never the real campaign. Anything that differs from this page is the result: write it down, do not work around it.

**Stop** (save the outputs, quit):
- CK3 crashes, stutters badly or shows an error popup after a click: write what was clicked and the exact text, **do not repeat the click** (the watcher saves on Ctrl+C).
- An unexpected permission prompt (any app, also Terminal asking for Documents): write the exact text and the app name **before** answering it.
- `errlines` shows a line about this mod (a `curia_` identifier or path): stop clicking, write every such line (cut to 260 characters, kept local). A line plainly about something else is a note.
- The achievements line reads "Not available" in a plain launch: check that debug mode is off, then write exactly when it changed and what was done just before.
- Launch 1 only: the console does not open in debug mode: write that, skip steps 9 and 10, go to step 11.

Anything else (an empty field, a missing line, a probe printing 0) is a result, not a stop.

## Before (CK3 and the launcher closed)
1. **Tabs.** Tab 1: `cd ~/PycharmProjects/curia`. Tab 2: `cd ~/PycharmProjects/curia && source tools/m3/session_env.sh` (sets the variables and the helpers `t11_snap`, `t11_check`, `errcount`, `errlines`).
2. **Checkout and tools** (tab 2).
   ```sh
   git switch main && git pull --ff-only
   ls mod/curia/descriptor.mod tools/m3/logwatch.py tools/m3/install_mod.sh docs/spikes/m3-protocol.md
   python3 --version
   python3 tools/m3/logwatch.py --selftest
   ```
   Must end with `SELFTEST_OK`; a missing file or a failure: stop and ask. *Write:* the Python version (R0).
3. **Back up the Continue pointer** (tab 2): `mkdir -p ~/curia_m3_backup && cp "$CK3/last_save.ck3" "$CK3/continue_game.json" ~/curia_m3_backup/ && ls -l ~/curia_m3_backup`
4. **Install** (tab 2): `tools/m3/install_mod.sh --dry-run`, `tools/m3/install_mod.sh`, `t11_snap A-before`. *Write (R0):* 11 files under `curia/` plus `curia.mod` (or the difference); `sw_vers -productVersion`; CK3 version (`grep -o '"rawVersion": *"[^"]*"' "$GAME/launcher/launcher-settings.json"`); number of displays; "Displays have separate Spaces" and Stage Manager (**look, do not toggle**).
5. **Launcher.** New **empty** playset `curia-m3`, add "Curia", make it active, debug mode **off**. Tab 2: `ls -l "$MODDIR" "$MODDIR/curia" "$MODDIR/curia/.metadata"`, `t11_snap A-after`, `diff ~/curia_m3_results/t11-A-before.sha.txt ~/curia_m3_results/t11-A-after.sha.txt` (empty = unchanged). *Write (R0, R7 row A):* Curia listed? enabled? any warning (exact text)? did the add-mod screen find it by itself? files intact? If the launcher removed them: quit it, rerun `tools/m3/install_mod.sh`, reopen, note it.

## Launch 1: T10, debug mode, throwaway game (nothing else is measured; no watcher)
6. `touch ~/curia_m3_results/t10-marker` (tab 2).
7. **Launcher: debug mode ON** (Game Settings). *Write (R1):* where the setting is and what it is called. Play. At the main menu, **before New game**: read the display mode (R0), set **Fullscreen**. Then **New game**, Ironman off, single player, any bookmark, save `curia_m3_t10_throwaway`. *Write:* the Achievements line at the setup screen.
8. **Console** (`` ` ``): type only `script_docs`, then `dump_data_types` (if "unknown command": `DumpDataTypes`; `help` once is allowed). *Write (R1):* the spelling that worked, the reply texts, how long each took, any freeze. Close the console, quit with the game's own menu. (R1 "needed `-debug_mode`": the console was reached in debug mode only, unless step 20 adds more.)
9. **Where did it write?** (tab 2; if it prints nothing because of a path problem, rerun with only `"$CK3"`)
   ```sh
   find "$CK3" "$GAME" "$HOME/Library/Application Support/Paradox Interactive" -type f -newer ~/curia_m3_results/t10-marker -exec stat -f '%z bytes  %Sm  %N' -t '%H:%M:%S' {} + 2>/dev/null | sort -k4
   ```
   Ignore the noise (normal logs, the new save, `last_save.ck3`, `continue_game.json`, `pdx_settings.txt`, `console_history.txt`, launcher `.sqlite` files, crash and dump folders). *Write (R1):* folder, names, sizes, times of every other new file; whether the folder existed before; "nothing new" if so (do not guess).
10. **Copy the dumps** (tab 2; replace `<path from step 9>` with each dump file or folder; never the save, the launcher database or `pdx_settings.txt`; a file in doubt is not copied, ask):
    ```sh
    mkdir -p reference/ck3 && git check-ignore -v reference/ck3
    cp -R "<path from step 9>" reference/ck3/
    ls -lR reference/ck3
    ```
    If `git check-ignore -v` prints nothing: **stop, copy nothing.** *Write (R1):* what `git check-ignore` printed; names and sizes only (`wc -l` per file is allowed); never open or paste a dump.
11. **Launcher: debug mode OFF**, confirm it reads off. *Write (R0):* confirmed off.

## Launch 2: plain Play (T5, T4, T3, T7)
12. **Before the launch.** Tab 2: `stat -f '%N  inode %i  size %z  modified %Sm' "$LOGS/debug.log" "$LOGS/error.log"` and `head -c 3 "$LOGS/debug.log" | od -An -tx1` (`ef bb bf` = a UTF-8 byte-order mark). Tab 1: `python3 tools/m3/logwatch.py --label before_launch`, leave it running. *Write (R2):* inode, size, modified of both logs; the three bytes (or "file missing").
13. **Launch CK3** (Play, playset `curia-m3`, debug off) and watch tab 1. *Write (R2):* for `debug.log` and `error.log` the event kind (`truncated` / `recreated` / `disappeared` / `appeared`; `attached` is not an event), old and new size, time. At the main menu repeat the `stat` line (tab 2): same inode and a smaller size = truncated in place, other inode = recreated. *Write (R0):* Fullscreen confirmed. Tab 1: type `newgame` + Enter.
14. **New game** (throwaway, Ironman off, single player): a count or higher, with vassals, already at war, ideally an heir; prefer names with characters outside plain ASCII (þ ð æ ø å ä ö é è č š ž ł). Read the Achievements line; save `curia_m3_throwaway`; start. About 15 s later (tab 2): `t11_check A-game`. *Write:* ruler and title, start date, vassal count, heir (holds land?), wars, claims, factions (R3); the LOAD count (R7 A); the achievements text (R0, R8 row 1).
15. **Gate.** Tab 2 `errcount` (0 expected; a nonzero count: read `errlines` first). Pause the game. Read the achievements indicator in the pause menu (second icon beside Game Rules) and in the Game Rules window (R8 row 1). *Write (R3):* both buttons ("Advisor", "Curia probes") visible? where? overlap with the HUD, and is the second button clear of the first (Cmd+Shift+3 screenshot, kept local)? hover highlight, tooltip text; does a click produce a line in tab 1; BEGIN name vs your ruler; BEGIN date vs the HUD date; `FIN` gold vs the HUD gold (read both at that moment). If the Advisor button is not visible or clickable: stop and write why.
16. **First look at the data** (tab 2, after one Advisor click and one Curia probes click, game paused):
    ```sh
    RAW=$(ls -t ~/curia_m3_results/raw-curia-*.txt | head -1)
    LC_ALL=C sed 's/^.*CURIA1|/CURIA1|/' "$RAW" | cut -c1-240
    LC_ALL=C grep -a -c '[^ -~]' "$RAW"
    LC_ALL=C grep -a -o '[^|]*[^ -~][^|]*' "$RAW" | sort -u | head -5 | xxd
    LC_ALL=C sed 's/^.*CURIA1|/CURIA1|/' "$RAW" | awk -F'|' '{print $3, NF}' | sort | uniq -c
    ```
    *Write (R5):* every line starts `CURIA1|<number>|`; the date's form; which fields print empty or 0; war-name markup; `none` lines; non-ASCII names and how `xxd` shows them. Field counts per kind should be BEGIN 7, FIN 4, HEIR 7 (4 for none), VASSAL 8 (5), WAR 6 (5), CLAIM 5, PROBE 5, END 4, LOAD 5; any other count: write it. **No non-ASCII name in this realm:** write "T4 charset: no non-ASCII name found; re-check in M4" (it stays open; conclude nothing about other names); the optional second new game (full sheet 2.9) can be added at the end of launch 2.
17. **Click protocol (T3).** For each label in this order: tab 1 type the label + Enter, wait for `label -> <label>`; Cmd+Tab to CK3 (no map click), set the state; **12 Advisor clicks, then 4 Curia probes clicks**, gaps irregular (about 4 to 10 s, never a beat), counted on paper; wait for the END line in tab 1 plus about 5 s. If any click showed no line within 60 s: keep the label, type `!<label>: N clicks, 0 lines seen` in tab 1, wait 60 s more and note whether lines arrived late and under which label, then go on.
    | Label | State |
    |---|---|
    | `paused` | paused (Space), no window open |
    | `menu` | pause menu (Esc) open. First: is the button visible, and does a click make a line appear? If not, write "not clickable in the pause menu" and use this route for every click: click while paused, press Esc about a second later, keep the menu open until the END line shows (the other display shows it), Esc, next click |
    | `after_unpause` | paused; press Space, click at once, press Space again; repeat per click |
    | `normal` | unpaused, speed 3 (key 3); close event windows, noting them |
    | `fast` | unpaused, speed 5 |

    *Write (R4):* clicks made per button per label; clicks with no line; for `menu` the route used; stutter, popup or sound; whether the game keeps running while the Terminal is in front.
18. **End of the run.** Tab 1: **Ctrl+C** (prints the summary and three file paths). Tab 2:
    ```sh
    JSON=$(ls -t ~/curia_m3_results/watch-*.jsonl | head -1)
    python3 tools/m3/frame_table.py "$JSON"
    errcount
    ```
    Nonzero `errcount`: read `errlines` first (stop rules). *Write:* from the summary R4 (section 2), R5 (sections 3, 4, 9), R2 (sections 1, 5), the clock offset (section 8), R6 (section 7: bytes appended from the launch to the end of the run, the CURIA part, the per-hour figure as printed; this window is not normal play); from `frame_table.py` the frames per label and button against your tally (a frame with snap number 0 and one line is the game-start LOAD line, not a defect), lines and bytes per export, whether END counts all lines or only the body (the summary prints both), any line over 4000 bytes, any line needing more than one read; the `errcount` (R8 row 2).
19. **Achievements and save/load.** Read the indicator again (pause menu, Game Rules; R8 row 2). Tab 1: `python3 tools/m3/logwatch.py --label saveload --duration 300`; save `curia_m3_throwaway`, load it, click Advisor once. *Write (R8 notes):* did saving after clicks and loading work; after the load does a click still produce a frame; does the snapshot number continue or restart; did a LOAD line appear after the load.
20. **Optional, last:** press `` ` `` once in this plain launch (no command). *Write (R1):* did a console open. Then quit CK3.

## After
21. **Send back, privately** (the files hold the game's own log text; not committed or posted): your filled tables (R0, R1 to R6, R7 row A, R8 rows 1 and 2 and the notes row); from `~/curia_m3_results/`: `summary-*.txt`, `watch-*.jsonl`, `raw-curia-*.txt`, `t11-A*.txt`, `t11-A*.sha.txt`, `t10-marker`; any text written down under the stop rules; from `reference/ck3/` names and sizes only. Screenshots stay local unless asked for.
22. **Cleanup** (CK3 and the launcher closed). Debug mode confirmed off. Optional: remove the spike mod (tab 2: `tools/m3/install_mod.sh --uninstall`, then `ls -l "$MODDIR"` should list only the workshop `.mod` files) and delete the `curia-m3` playset in the launcher after switching back to your usual playset; or keep both in place if more M3 checks may follow. Either way confirm the achievements indicator reads "Available" at a plain Play's new-game setup screen. Delete the throwaway saves if wanted. If Continue lost the real campaign: `cp ~/curia_m3_backup/last_save.ck3 ~/curia_m3_backup/continue_game.json "$CK3/"`.
