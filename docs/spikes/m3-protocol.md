# Milestone 3 session protocol (owner, macOS 26.5.1, CK3 1.20.0.3)

Status: **prepared, not yet run.** Nothing in this sheet is a result: every "does it work" below is answered by running it and reading the tool's output (runtime facts are measured, never estimated); no number is estimated. Sources: `docs/milestones.md` (M3), `docs/research-notes.md` §2.1, §2.2, §2.14, §5.1, `docs/spikes/m0-results.md`, `docs/spikes/m2-results.md`; lessons from `m0b-checklist.md` and `m2-checklist.md`.

The spike mod is `mod/curia/` (build 0.1.0; description and line frame in `mod/curia/README.md`), installed with `tools/m3/install_mod.sh`, and the measuring tool is `tools/m3/logwatch.py` (description in `tools/m3/README.md`).

**Two sessions.** Session 1 (Parts 0, 1, 2 and 5: two launches) answers what the wire-format freeze needs. Session 2 (Parts 3 and 4: the long play stretches and the metadata variants) can be a different day; Part 3 and Part 4 are independent halves and may also be split across days. Each sitting starts with its own preflight and ends with its own cleanup.

**Roles.** "The owner" is the person at the Mac. "The maintainer" is the person who commits and reads the results.

## Scope
| Question | Answered by | Part | Session |
|---|---|---|---|
| **T10** `script_docs` / `dump_data_types` on the Mac: spelling, where the output lands, does it need debug mode | a debug-mode launch in a throwaway game, **its own launch, nothing else is measured in it** | 1 | 1 |
| **T5** is `debug.log` truncated or recreated at launch; what a tailer sees | watcher started before CK3 | 2 | 1 |
| **T4** line format, length, charset, END count | watcher plus the raw lines | 2 | 1 |
| **T3** flush latency per condition (paused, menu, after unpause, normal, fast), both buttons | watcher plus a click protocol | 2 | 1 |
| **T7** (repeat with the real mod) button visible, clickable, root scope, Fullscreen | in-game | 5 | 1 |
| **T11** which launcher metadata the macOS launcher needs; behaviour of `supported_version` | row A in Session 1, rows B to G as variant installs | 4 | 1 and 2 |
| **T8** bytes per export, bytes per hour of play, baseline growth | watcher | 3 | 2 (per-export bytes come from Session 1) |
| **C3** the 17MB cumulative cap | **not run in this sheet**; how to start it is in 3.6 (deferred, not failed) | 3 | none |
| **T12** ck3-tiger | done by the maintainer before the sessions; the owner does nothing | 4 | none |

Not in this session: T0, T1, T2, T6 (answered in M0, not repeated), T9 (optional clipboard comparison, skipped), CK3 **Windowed** (deferred to the M4 testing, as in M2), Windows and Linux (CI plus testers later). Also not measured here, so left open in `research-notes.md` §5.1 (the maintainer marks them open, not answered): T3 whether OS file-change notifications fire (A15; the watcher polls) and buffered versus line-flushed as a mechanism (T3 here measures the delay per condition only); T4 whether quotes, braces and newlines are preserved (only non-ASCII bytes and the byte-order mark are looked at; the 8,000-character line was M0b) and the maximum line length and truncation beyond 8,000 characters (the mod's lines are far shorter); T7 in the loading screen, in observer mode and in an Ironman game (map view and pause are covered). Everything here is **macOS arm64 only**; log behaviour on other systems stays unverified.

## Rules for the whole session
- **Normal launch (plain Play, debug mode off) for everything except Part 1.** Console use happens only in Part 1 and in the optional last step 7 of Part 5, both in throwaway games, both maintenance tests and never user-facing steps (Curia itself never asks a user to use the console); a game that had console use loses achievements (a debug-mode launch already showed "Not available" in M0).
- Throwaway games only: `curia_m3_t10_throwaway` (Part 1), `curia_m3_throwaway` (Parts 2 and 5), `curia_m3_t8_throwaway` (Part 3), `curia_m3_t11_<row>` (Part 4). Never the real campaign, never a game with a cheat mod in the playset.
- **Two Terminal tabs.** Tab 1 only runs the watcher (labels and `!` notes are typed there). Tab 2 is for every other shell command. Each command below says which tab. While tab 2 is in front the watcher keeps running and keeps its output; switch back to tab 1 before typing a label.
- Dump files, raw log lines and screenshots stay on the Mac (`reference/ck3/` is gitignored; `~/curia_m3_results/` is outside the repository). The tables in this sheet take words and numbers, **not game text**: never paste dump content or engine log lines into an issue, a document or a chat (no Paradox-generated content in the repository).
- Anything that turns out different from what the sheet assumes is the result: record it, do not work around it.
- Expectations are written as questions. An empty cell means "not observed", not "no".

## Order of the day
| Step | Launch | What |
|---|---|---|
| **Session 1** | | |
| S1-0 | none (CK3 closed) | Part 0: preflight, install the mod, playset |
| S1-1 | **launch 1, debug mode, throwaway game** | Part 1 (T10), nothing else; then turn the debug-mode setting off and confirm it |
| S1-2 | **launch 2, plain Play** (watcher running before the launch) | Part 2 (T5 at launch, T4, T3). Part 5 steps 1 to 3 at the gate 2.4, steps 4 and 5 after 2.8, then step 6, then the optional 2.9, then the optional step 7 last |
| S1-3 | none | end of Session 1: send back now, cleanup of the debug-mode setting |
| **Session 2** | | |
| S2-0 | none | Session 2 preflight |
| S2-1 | launch 3, plain Play | Part 3 (T8 hour and baseline stretches), Part 5 steps 4 and 5 again |
| S2-2 | launches 4 to 9, plain Play, one per row | Part 4 rows B to G (row A was launch 2) |
| S2-3 | none | final cleanup, send back |

Part 5 keeps the milestone text's number; it sits after Part 2 because it runs inside launch 2.

## Protocol parameters
These are **chosen values of the protocol**, not predictions. The tool reports min / median / max per label, so the sample size stays visible, and any condition can be repeated.

| Parameter | Value | Why |
|---|---|---|
| Advisor clicks per condition | 12 | a protocol choice |
| Curia probes clicks per condition | 4 | the probe export is larger |
| Gap between clicks | irregular, about 4 to 10 s, never a fixed beat, never counted | the engine stamps whole seconds; irregular gaps spread the clicks over sub-second phases |
| Hour stretch (T8) | 60 min, one Advisor click about every 5 real minutes (minutes 5, 10, ... 55) | per-hour growth in normal use |
| Baseline stretch (T8) | 15 min, **no clicks**, same game speed as the hour stretch | the game's own logging |
| Game speed in the T8 stretches | speed 3 (digit key 3), written down and kept the same in both stretches | a protocol choice |
| "normal" and "fast" conditions | speed 3 and speed 5 (digit keys; M0: key 5 is top speed) | |
| Wait after the last click of a condition | until the terminal shows the END line of that export, plus about 5 s | a late line must not be filed under the next label |
| No-line limit | a click with no line in the terminal within 60 s is counted as "no line" in the tally; if a condition has any such click, see 2.6 before changing the label | the limit is a protocol choice |

## Stop rules
- **CK3 crashes**, stutters badly or shows an error popup after a click: stop, write down what was clicked and the exact text, save the outputs (the watcher saves on Ctrl+C), do not repeat the click.
- **A permission prompt you did not expect** (any app, including Terminal asking for the Documents folder): write down the exact text and the app name it shows before answering it.
- **error.log lines about this mod:** the watcher does not print them live (only in its summary and `.jsonl`). The check is the case-insensitive pattern `curia[_/]` (vanilla files contain `curia` inside other words, so the bare word is not used); `errcount` (0.1) prints the count. A nonzero count is **first read line by line** (`errlines`, on screen, kept local). Only a line about this mod (a `curia_` identifier of this mod or its path) stops the session: stop clicking, record every such line (cut to 260 characters), save the outputs and quit. A line that is plainly about something else is written down as a note and the session goes on. Check at the points marked "error.log check".
- **The achievements indicator reads "Not available" in a plain-Play game** at any point: first look at the debug-mode setting in the launcher (it must be off), then stop and write down exactly when it changed and what was done just before (without a debug-mode cause this would contradict M0).
- **Part 1 only:** if the console does not open in debug mode, record that and skip the rest of Part 1.
- Everything else (an empty field, a missing line, a probe printing 0) is a result, not a stop.

# Session 1

## Part 0. Preflight (CK3 and the launcher closed)

**0.1 Terminal setup.** Two tabs; with two displays, put this Terminal on the display that is **not** CK3's (typing labels needs it; M2 used the same layout). Adjust the `cd` if the checkout lives elsewhere.

Tab 1 (watcher only):
```sh
cd ~/PycharmProjects/curia
```
Tab 2 (all other commands; paste the block, then the helper functions):
```sh
cd ~/PycharmProjects/curia
CK3="$HOME/Documents/Paradox Interactive/Crusader Kings III"
LOGS="$CK3/logs"; MODDIR="$CK3/mod"
GAME="$HOME/Library/Application Support/Steam/steamapps/common/Crusader Kings III"
mkdir -p ~/curia_m3_results
```
The helper functions only read the logs and the mod folder and write small files under `~/curia_m3_results/`. `t11_snap <tag>` stores a checksum list of the mod folder; `t11_check <tag>` prints and saves counts from the logs (counts of lines, not their text); `errcount` and `errlines` are the error.log check of the stop rules:
```sh
t11_snap() { find "$MODDIR" -type f ! -name .DS_Store -exec shasum -a 256 {} + | sort -k2 > ~/curia_m3_results/t11-$1.sha.txt; }
errcount() { grep -a -i -c -E 'curia[_/]' "$LOGS/error.log"; }
errlines() { grep -a -i -n -E 'curia[_/]' "$LOGS/error.log" | cut -c1-260; }
t11_check() {
  {
    echo "== T11 check, label $1, $(date '+%Y-%m-%d %H:%M:%S')"
    echo "LOAD lines in debug.log (CURIA1|0|LOAD|): $(grep -a -c 'CURIA1|0|LOAD|' "$LOGS/debug.log")"
    echo "other debug.log lines mentioning curia (CURIA1 lines excluded; a wide match, vanilla words can count): $(grep -a -i curia "$LOGS/debug.log" | grep -a -v -c 'CURIA1|')"
    echo "error.log lines matching curia[_/]: $(errcount)"
    echo "debug.log: $(stat -f '%z bytes, inode %i' "$LOGS/debug.log"); error.log: $(stat -f '%z bytes' "$LOGS/error.log")"
  } 2>&1 | tee ~/curia_m3_results/t11-$1-$(date +%Y%m%d-%H%M%S).txt
}
```

**0.2 Checkout** (tab 2). Switch the local checkout to `main` and pull, then check the M3 files are there:
```sh
git switch main && git pull --ff-only
ls mod/curia/descriptor.mod tools/m3/logwatch.py tools/m3/install_mod.sh docs/spikes/m3-protocol.md
```
If any file is missing the M3 spike is not merged into `main` yet: stop and ask the maintainer which branch to use.

**0.3 Tools** (tab 2). `python3 --version` (the watcher needs 3.9 or newer; record the version). Then:
```sh
python3 tools/m3/logwatch.py --selftest
```
It must end with `SELFTEST_OK` (it uses a temporary folder and a simulated writer, not CK3). Optional: `python3 -m unittest discover -s tools/m3` must end with `OK`. If either fails: stop and send the output to the maintainer. No build is needed.

**0.4 Back up the Continue pointer** (tab 2; throwaway runs share it; the M0 lesson). Only the two pointer files are copied (the save folder was 1.3 GB at last look):
```sh
mkdir -p ~/curia_m3_backup && cp "$CK3/last_save.ck3" "$CK3/continue_game.json" ~/curia_m3_backup/ && ls -l ~/curia_m3_backup
```

**0.5 Record the state before the session** (table R0): macOS version (`sw_vers -productVersion`), CK3 version (`grep -o '"rawVersion": *"[^"]*"' "$GAME/launcher/launcher-settings.json"`, tab 2), number of displays, System Settings → Desktop & Dock → Mission Control: "Displays have separate Spaces" and Stage Manager (**record, do not toggle**). **Display mode:** CK3 is closed in this part, so it is read and set later: read the Graphics display mode in the in-game settings at the main menu of launch 1 and set **Fullscreen** there; at the main menu of launch 2 confirm it again before the new game (R0 takes both readings). The achievements indicator is read at the new-game screens of launches 1 and 2 (Parts 1 and 5).

**0.6 Install the mod** (tab 2; CK3 fully quit: the script refuses to run while the process `ck3` exists):
```sh
tools/m3/install_mod.sh --dry-run
tools/m3/install_mod.sh
```
The dry run changes nothing; the real run copies `mod/curia` to `$MODDIR/curia` (the repository copy is never touched), writes the outer `$MODDIR/curia.mod`, then prints the installed file list and the outer file. The list must show **11 files under `curia/`** (`descriptor.mod`, `.metadata/metadata.json`, `README.md`, the two `gui` files, five script files, the localization file) plus `curia.mod`. Default variant is `both`; the variants are Part 4. Record any difference. Then take the first checksum list (T11 row A): `t11_snap A-before`.

**0.7 Launcher: a playset with only Curia.** Open the Paradox Launcher. Create a **new, empty playset** (`curia-m3`), add "Curia" to it, make it active (an earlier launcher run **cleared the mod folder** and the add-mod screen's wording is not documented, so check again after the launcher is open). Tab 2:
```sh
ls -l "$MODDIR" "$MODDIR/curia" "$MODDIR/curia/.metadata"
```
The listing must still show `curia.mod`, `curia/descriptor.mod` and `curia/.metadata/metadata.json`; then `t11_snap A-after` and `diff ~/curia_m3_results/t11-A-before.sha.txt ~/curia_m3_results/t11-A-after.sha.txt` (empty output = the launcher changed nothing). If the launcher removed them: quit the launcher, run `tools/m3/install_mod.sh` again, reopen the launcher, and write down that it happened. Record in R0 (and, for the default install, as T11 row A in R7): is "Curia" listed, is it enabled, is there any warning or "outdated" mark and its exact text, did the add-mod screen find it by itself, what the screen was called.

**Pre-launch check (before every launch of both sessions; Part 4 rows excepted for the playset item, where the listing is itself the observation):** (1) the playset `curia-m3` is active and shows "Curia" enabled; (2) the **debug-mode setting in the launcher is off** (it is on only for launch 1; where it lives is in Part 1 step 2); (3) CK3 and, for an install, the launcher are closed as the step says. Do not launch yet.

## Part 1. T10: `script_docs` and `dump_data_types` (launch 1, debug mode, throwaway game)

**This launch is only for T10. No watcher, no Curia button, no latency or size measurement; nothing from this launch goes into the T3/T4/T5/T8 tables.** Part 1 is a maintenance test run by the owner in a throwaway game, never a user-facing step. The game started here loses achievements (console use disables them; a debug-mode launch already showed "Not available" in M0). It is separate because every other part needs a plain launch to keep the achievements indicator valid.

In the repo (`research-notes.md` §2.2, §2.3, C20): the wiki says `script_docs` writes to the `logs` folder and the data-types dump writes `data_types.log` there, and spells the second command `DumpDataTypes` on one page and `dump_data_types` on another; in M0 the console key was `` ` `` and `help` worked. Whether the output lands there **on the Mac** is not known; step 1 finds out.

1. **Find out where.** Create a time marker (tab 2), to be compared after the commands have run:
   ```sh
   touch ~/curia_m3_results/t10-marker
   ```
2. **Turn debug mode on in the launcher.** M0 run C started debug mode from the launcher's Game Settings, so it is a launcher setting, and it is treated as **persistent until seen off again**; write down where the entry is and what it is called. Use the menus only until step 3: **new game**, Ironman off, single player, any bookmark, save name `curia_m3_t10_throwaway`. Record what the achievements indicator reads at the setup screen (M0: "Not available" immediately in a debug-mode launch). At the main menu set the display mode to Fullscreen (0.5).
3. Once the game is running, open the console with `` ` ``. Type **only** these, in this order, one Enter each, waiting for each to finish (watch for a freeze, and note how long it took in words):
   - `script_docs`
   - `dump_data_types`; if the console says the command is unknown, try `DumpDataTypes`. Record the exact spelling that worked and the console's reply text.
   Do not type anything else (if the spelling must be looked up, `help` is allowed once; it only lists commands). Close the console, quit the game with its own menu (some tools write at exit; the listing below catches that).
4. **List what was written** (tab 2; names, sizes and times only; this is the answer to "where"):
   ```sh
   find "$CK3" "$GAME" "$HOME/Library/Application Support/Paradox Interactive" -type f -newer ~/curia_m3_results/t10-marker -exec stat -f '%z bytes  %Sm  %N' -t '%H:%M:%S' {} + 2>/dev/null | sort -k4
   ```
   The listing also shows entries that are not dumps: the normal logs that grew in the same minutes (`debug.log`, `error.log`, `game.log` and others), the new save, `last_save.ck3`, `continue_game.json`, `pdx_settings.txt`, `console_history.txt`, the launcher's `launcher-v2*.sqlite` files and anything under crash or dump folders. The dump files are the entries that appeared under `logs` (or in a new folder next to it) at the time of the two commands and are not on that list. Write down the **folder and file names and sizes of every new file that is not on the list**, and whether the folder already existed. If nothing new appears, say so; do not guess a location. If `find` prints nothing at all because of a path problem, rerun it with only `"$CK3"`.
5. **Copy into `reference/ck3/`** (tab 2; gitignored; the folder is created here; replace `<path from step 4>` with each dump file or folder, keep the quotes; never the save, the launcher database or `pdx_settings.txt`; a file in doubt is not copied, ask the maintainer):
   ```sh
   mkdir -p reference/ck3 && git check-ignore -v reference/ck3
   cp -R "<path from step 4>" reference/ck3/
   ls -lR reference/ck3
   ```
   `git check-ignore -v` must print the `.gitignore` rule for `/reference/`; if it prints nothing, stop and do not copy anything (the rule must hold before any dump is stored). Record **file names and sizes only** (`ls -l`); a line count per file (`wc -l`) is allowed. **Never open the dump in a document that is shared, never paste any of it anywhere.**
6. Record whether the commands needed `-debug_mode`: in this launch the console was reached **in debug mode only**. Whether the commands work without it is not tested here (the console is not opened in a plain launch to keep Part 5's achievements check clean); the optional step 7 of Part 5 closes that at the end of launch 2.
7. Quit CK3. **Turn debug mode off again in the launcher (Game Settings) and confirm it reads off before launch 2.** Delete the throwaway save from Load Game if wanted. The launch-2 setup screen is the check: the achievements indicator must read "Available" there (M0 reference); the M0 run recorded no warning mark in debug mode, so a mark is not the signal.

## Part 2. Launch 2: T5 (launch), T4 (format), T3 (latency)

Plain Play, playset `curia-m3`, Fullscreen, debug mode off (pre-launch check). The watcher runs in tab 1 on the other display and is started **before** CK3.

**2.1 Before the launch** (CK3 closed). Record the log files' identity independently of the watcher, so T5 has a second witness (tab 2):
```sh
stat -f '%N  inode %i  size %z  modified %Sm' "$LOGS/debug.log" "$LOGS/error.log"
head -c 3 "$LOGS/debug.log" | od -An -tx1
```
(The second line shows a UTF-8 byte-order mark, `ef bb bf`, if present; if the file does not exist, say so.) Then start the watcher (tab 1):
```sh
python3 tools/m3/logwatch.py --label before_launch
```
It prints `watching ... results in ...`. If `debug.log` exists it attaches at its end (default; existing content is not counted) and prints an `attached` line; if not, it prints `waiting for debug.log`. Leave it running. **Labels:** type a word and Enter in tab 1 to tag what the game is doing; every CURIA line is filed under the label current when it arrived. A line starting with `!` is a free-text note. The window prints `label -> <word>` or `note recorded: ...` as confirmation.

**2.2 Launch CK3** (launcher, Play, playset `curia-m3` enabled, debug mode off). Watch tab 1 while the game starts: it prints one line per file event, `<time> debug.log: <kind> (old size ... B, new size ... B)`. The kinds are `attached` (an existing file seen when the watcher started; not a T5 event, only note its size), `truncated`, `recreated`, `disappeared` and `appeared`; a separate `waiting for ...` line means the file did not exist yet. **T5: write down which kind appeared for `debug.log` and for `error.log` after CK3 started, the old and new size, and the time.** When the main menu shows, repeat the `stat` line of 2.1 (tab 2) and compare the inode and size with the "before" values (same inode and a smaller size = truncated in place; a different inode = recreated). Confirm the display mode is Fullscreen (0.5). Then type the label `newgame` in tab 1 (so that the game-start line is filed under it).

**2.3 New game** (throwaway, Ironman off, single player, Fullscreen). Choose a ruler like in M0b: **a count or higher, with vassals, already at war at the bookmark, ideally with an heir**; factions and claims are optional. For T4, prefer a realm whose names contain characters outside plain ASCII (the ruler, the heir, the top vassals or the claimed titles): look for letters such as þ ð æ ø å ä ö é è č š ž ł (Norse, Slavic, Greek, Arabic or Occitan areas are a guess; the first export is the real check, 2.5). On the setup screen read the **Achievements line right of the Ironman checkbox** (R0). Name the save `curia_m3_throwaway`. Start.
Once the game has started (about 15 s later), run `t11_check A-game` (tab 2; T11 row A: is the `LOAD` count 1, as expected for a new game). Write down (table R3; so that empty fields can be explained): character and title, start date (HUD), number of vassals (Vassals window), heir name and whether the heir holds land, wars (names), the ruler's claims, any faction against the ruler.

**2.4 Gate: Part 5 steps 1 to 3 now, and the error.log check** (`errcount` in tab 2, R8 row "start of the click protocol"; a nonzero count is read with `errlines` first, stop rules). Is the button there, does a click work, is the root the player. If the Advisor button is not visible or not clickable, stop the click protocol and record why (the rest of Part 2 needs the button).

**2.5 First look at the data** (tab 2; after the first Advisor click and one Curia probes click, game paused): the lines are in the raw file the watcher writes. Show them without the engine prefix (on screen only):
```sh
RAW=$(ls -t ~/curia_m3_results/raw-curia-*.txt | head -1)
LC_ALL=C sed 's/^.*CURIA1|/CURIA1|/' "$RAW" | cut -c1-240
```
Questions for R5 (describe in words, no pasting): does every line start with `CURIA1|<number>|`, is the snapshot number filled in (the mod reads it through `EmptyScope`, proven only for a constant), does the BEGIN line show the ruler's name, title and the date, in what form is the date (spaces, commas, empty), which fields print empty or `0` in FIN, HEIR, VASSAL, WAR, CLAIM and PROBE lines, do the `WAR` lines show a link-markup form of the war name, do `none` lines appear where the realm has nothing (heir, wars, claims). To list the non-ASCII names, if any:
```sh
LC_ALL=C grep -a -c '[^ -~]' "$RAW"
LC_ALL=C grep -a -o '[^|]*[^ -~][^|]*' "$RAW" | sort -u | head -5 | xxd
```
The first command counts CURIA lines that contain a byte outside printable ASCII; the second shows the fields concerned as bytes. (No `#` comments inside pasted blocks: the default macOS shell does not accept them interactively.)
The `xxd` view shows how a character is encoded (for example the two bytes `c3 9e` for one letter would be UTF-8). If no non-ASCII name exists in this realm: record "none in this realm"; T4's charset question then stays open for a second look (see 2.9); do not conclude anything about other characters.
Field counts per line kind (does any kind vary unexpectedly):
```sh
LC_ALL=C sed 's/^.*CURIA1|/CURIA1|/' "$RAW" | awk -F'|' '{print $3, NF}' | sort | uniq -c
```
Counting the leading `CURIA1` and the snapshot number as fields, the README's frame gives these counts: `BEGIN` 7, `FIN` 4, `HEIR` 7 (or 4 for `HEIR|none`), `VASSAL` 8 (or 5 for `VASSAL|0|none`), `WAR` 6 (or 5 for `WAR|0|none`), `CLAIM` 5, `PROBE` 5, `END` 4, `LOAD` 5. Any other count for a kind: record it (a name containing the separator would show up here).

**2.6 The click protocol (T3).** Paper tally first: for every condition keep a count of clicks made on each button. Conditions in this order, labels exactly as written. For each condition:

1. Switch to tab 1 (Cmd+Tab to Terminal), type the **label**, Enter, wait for `label -> <label>`.
2. Cmd+Tab back to CK3 (no click on the map) and set the state in the table.
3. **12 Advisor clicks**, then **4 Curia probes clicks**, with irregular gaps (see the parameters). Count them on paper.
4. After the last click wait until tab 1 shows the END line of that export and then about 5 s more (no-line limit: 60 s per click; a click that never shows a line goes into the tally as "no line").
5. **If any click of the condition showed no line:** keep the label, type a note in tab 1 (for example `!paused: 16 clicks, 0 lines seen`), then wait one more 60 s and note whether lines arrived. Only then change the label. Lines that arrive late are filed under the label current when they arrive; the note is how R4 attributes them to the condition they belong to (record the attribution in R4).
6. Next condition.

| Label | State of the game when each click is made |
|---|---|
| `paused` | paused (Space; the HUD shows it), no window open |
| `menu` | the pause menu (Esc) open. **First answer a question:** with the menu open, is the Advisor button still visible, and is a click accepted (a line appears)? Branch A, clickable: click as usual with the menu open. Branch B, **not clickable** (the button hidden or covered): record "not clickable in the pause menu" as the result for this route, then use the other route: with the game paused and the menu closed, click the button, press Esc about a second later, leave the menu open until tab 1 shows the END line (the second display shows it), press Esc, next click. That condition measures a game that is in its menu while the line is waiting to be written, and the table says which route was used |
| `after_unpause` | start paused; press Space, click the button as soon as possible (aim for the first second or two), press Space again to pause; repeat per click |
| `normal` | unpaused at speed 3, without touching the pause between clicks; if an event window pauses the game, close it and continue, noting it |
| `fast` | unpaused at speed 5; as above |

Observations to write down for every condition (R4): does the game visibly keep running while the Terminal is in front (relevant to the `normal` and `fast` labels, typed in the Terminal), does any click cause a visible stutter, any popup, any sound.

**2.7 End of the T3 run.** In tab 1 press **Ctrl+C**. It prints the summary and the three saved paths (`summary-*.txt`, `watch-*.jsonl`, `raw-curia-*.txt`, all in `~/curia_m3_results/`). Read it for R4 (summary section 2), R5 (sections 3, 4, 9) and R2 (sections 1, 5). Note that sample counts in section 2 are **lines**, not clicks; the frames are the clicks. Frames beyond the first 40 appear only in the `.jsonl`, so tabulate all of them (tab 2; also gives bytes per export per button for Part 3):
```sh
JSON=$(ls -t ~/curia_m3_results/watch-*.jsonl | head -1)
python3 - "$JSON" <<'EOF'
import collections, json, statistics, sys
frames = []
with open(sys.argv[1], encoding="utf-8") as f:
    for line in f:
        rec = json.loads(line)
        if rec["type"] == "frame":
            rec["button"] = "probes" if "PROBE" in rec["kinds"] else "advisor"
            frames.append(rec)
print("snap | button | label | lines | END says | text bytes | file bytes | longest line | span s | complete")
for r in frames:
    print(r["snap_id"], r["button"], r["label"], r["lines"], r["declared_count"], r["payload_bytes"],
          r["raw_bytes_with_prefix"], r["longest_line_bytes"], r["span_s"], r["complete"], sep=" | ")
print("\ncomplete frames per label and button (compare with the click tally):")
for key, n in sorted(collections.Counter((r["label"], r["button"]) for r in frames if r["complete"]).items()):
    print(" ", key[0], key[1], n)
print("\nper button, complete frames (min / median / max):")
for button in ("advisor", "probes"):
    rows = [r for r in frames if r["complete"] and r["button"] == button]
    for k, name in (("lines", "lines"), ("payload_bytes", "text bytes"), ("raw_bytes_with_prefix", "file bytes")):
        v = [r[k] for r in rows]
        if v:
            print(f"  {button} {name}: {min(v)} / {statistics.median(v)} / {max(v)} (n={len(v)})")
EOF
```
Questions for the output: a frame with `snap_id` 0, one line and "no BEGIN; no END" is the game-start line `CURIA1|0|LOAD|...`, not a defect. Does the END count equal all lines of the frame or only those between BEGIN and END (the summary prints both; the README states "all lines")? Do the lines of one export arrive together (frame `span s` near 0) or spread over time? Is a frame ever interleaved with other output or another frame? Did any line need more than one read? Is any line over 4000 bytes? Is the engine prefix the same shape for every line, and how many bytes precede the marker (summary section 3, min / median / max)?

**2.8 error.log check** (tab 2; after the protocol, R8 row "after the protocol"):
```sh
errcount
```
`0` expected as a question to answer. Any other number: read the lines with `errlines` first and apply the stop rule. The watcher's own count (it uses the same pattern) is `grep -c '"type":"error_hit"' "$JSON"`; for this run `JSON` is the file set in 2.7, for any later run set `JSON=$(ls -t ~/curia_m3_results/watch-*.jsonl | head -1)` again.

**2.9 Optional, if no non-ASCII name appeared** (one more new game; after Part 5 steps 4 to 6, before step 7): in tab 1 start a **new watcher run** first (`python3 tools/m3/logwatch.py --label game2`; the snapshot counter lives in the save, so a second game restarts at 1 and must not share a run with the first), start another **new game** at a different bookmark (same launch; the extra start-up cost shows in R6), click Curia probes twice, repeat 2.5, end the run with Ctrl+C. Otherwise record "T4 charset: no non-ASCII name found; re-check in M4".

## Part 5. T7 repeat with the real mod, error.log, achievements (run inside launch 2)

The first pass was the M2 stub (`m2-results.md`: Fullscreen, visible, clickable, nine clicks unpaused and paused, no tooltip). Here it is repeated with the real mod; Windowed stays deferred.

1. **Visible?** In Fullscreen, after the new game has loaded: are both buttons ("Advisor" and "Curia probes") visible at the bottom left of the HUD, where, do they overlap any HUD element (screenshot with Cmd+Shift+3, kept local), and is the second button's position (never seen on screen before) clear of the first button and the HUD? Compare with the M2 stub's position (it sat at the Advisor button's position). The two buttons span x 570 to 866 on the owner's 2056-point-wide display; that is a spike placement, not a final one.
2. **Clickable?** Does the cursor highlight a button on hover, does a tooltip appear (text read from the screen, not from the mod), and does a click produce a line in tab 1 (paused first, to keep the game still)?
3. **Root scope = the player?** Look at the BEGIN line (command in 2.5): is the name and title the ruler you are playing (**the owner's own answer**; M2 left this pending), and is the date the HUD date? Does the `FIN` gold match the HUD gold at that moment (read both)?
4. **error.log** (after Part 2, and again at the end of Part 3 in Session 2): `errcount` and the watcher summary's section 6. Does the mod load with no `curia[_/]` entry in `error.log` (milestone criterion 6)? Record the count and, for any hit, the line (cut to 260 characters, local).
5. **Achievements indicator with the real mod:** at the new-game setup screen (Achievements line next to the Ironman checkbox), in the pause menu (the second icon beside Game Rules; the first is Ironman), in the Game Rules window, once at the start and once after the click protocol (Session 1); in Session 2 once at the setup screen of launch 3 and once at the end of Part 3. Exact text of each. (M0 reference: plain launch "Available"; stop rule above if it reads "Not available".)
6. **Save and load** (after step 5, before 2.9 and step 7; tab 1: start a short run first, `python3 tools/m3/logwatch.py --label saveload --duration 300`). The mod keeps six counters in the save (`curia_snap_id`, `curia_export_lines`, `curia_rank`, `curia_war_index`, `curia_claim_index`, `curia_probe_mode`). Does saving the game after clicks work normally (save and load `curia_m3_throwaway`)? After loading it, does a click still produce a frame, and does the snapshot number continue (was it kept in the save) or restart? Does the load print a `LOAD` line (not known, observe)? (One minute; write the observation.)
7. **Optional, last step of launch 2, T10 follow-up:** whether `script_docs` needs debug mode at all. This opens the console in a **plain** launch, so do it only after every step above is finished and recorded, in this throwaway game (the game loses achievements if a command is run): press `` ` `` once. Does a console open? Close it, run **no command**. If a console opens, record it and stop there. If this step is skipped, T10's "needs debug mode" is recorded as "console reached in debug mode only".

## End of Session 1

**Send back now** (to the maintainer privately; these hold the game's own log text and are **not committed or posted**): the filled tables R0 (the Session 1 cells), R1 to R5, R7 row A and R8 (rows 1 and 2 and the saving-and-loading notes row); from `~/curia_m3_results/` all files of Session 1 (`summary-*.txt`, `watch-*.jsonl`, `raw-curia-*.txt`, `t11-A*.txt`, `t11-A*.sha.txt`, `t10-marker`); anything written down verbatim under the stop rules. From `reference/ck3/` only **names and sizes**. Screenshots stay local unless asked for.

**Cleanup of Session 1:**
- Confirm the **debug-mode setting in the launcher is off** (turned off after Part 1; confirmed before launch 2).
- Leave the mod installed and the `curia-m3` playset in place for Session 2 (the full cleanup is at the end of Session 2).
- Delete the throwaway saves `curia_m3_t10_throwaway` and `curia_m3_throwaway` in Load Game if wanted. If Continue no longer shows the real campaign: `cp ~/curia_m3_backup/last_save.ck3 ~/curia_m3_backup/continue_game.json "$CK3/"` (tab 2).

# Session 2

## Session 2 preflight (CK3 and the launcher closed; record in R0)
1. Tab 1: `cd` as in 0.1. Tab 2: paste the 0.1 block and the helper functions again.
2. `python3 tools/m3/logwatch.py --selftest` (tab 2) ends with `SELFTEST_OK`. If the checkout changed since Session 1, repeat 0.2 first.
3. **Mod installed?** `ls -l "$MODDIR" "$MODDIR/curia" "$MODDIR/curia/.metadata"` (tab 2) must show `curia.mod`, `curia/descriptor.mod` and `curia/.metadata/metadata.json`. If not, `tools/m3/install_mod.sh` (CK3 closed) and look again after the launcher is open (0.7).
4. Pre-launch check (0.7): playset `curia-m3` active with "Curia" enabled, and the **debug-mode setting in the launcher off**.
5. **Achievements baseline:** at the new-game setup screen of the first launch of this sitting the Achievements line must read "Available"; if "Not available", the stop rule applies. Display mode Fullscreen (confirm at the main menu).

## Part 3. T8: bytes per export, per hour, and the cap (launch 3)

Launch 3 (plain Play), then a **new game** `curia_m3_t8_throwaway` with a ruler like in 2.3 (so that exports are comparable with Part 2), then pause and start the watchers below once the game is running.

**3.1 Bytes per export.** From 2.7 (Session 1 files): bytes per export per button, "text bytes" (from the marker on) and "file bytes" (including the engine prefix, as the tool defines them), min / median / max, plus the line count. Record in R6. Also from the Part 2 summary, section 7: how many bytes `debug.log` and `error.log` grew from the launch to the end of the protocol (start-up burst, the new game start, the clicks) and how much of that was CURIA lines (an observation of the cost of one launch plus one new game; M0 saw about 1 MB per start-up and about 1 MB per further new game).

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

**3.5 error.log check again** (tab 2): `errcount` (set `JSON=$(ls -t ~/curia_m3_results/watch-*.jsonl | head -1)` again before comparing with the watcher's `error_hit` count), then Part 5 steps 4 and 5 (R8 row "end of Part 3").

**3.6 The 17MB cap (C3): not part of this session.** The claim (`research-notes.md` C3): CK3 may stop writing logs after about 17MB of **cumulative bytes written** by one game process, counted over `debug.log` and `error.log` together, not reset by truncating a file, reset only by restarting the game. A reproduction needs one very long play session with the watcher running; many exports cannot be produced in a loop without the console (each export is one manual click), so the cap cannot be forced quickly. It is **deferred, not failed**, until the per-hour rates of 3.4 are known. To start it later: start the watcher before CK3 (as in 2.1) and leave the game running with `python3 tools/m3/logwatch.py --report-every 300 --label long` in tab 1; the status line shows both file sizes and the bytes appended since the watcher started. The sign to look for is activity that stops producing lines while the game clearly runs and the sizes stop growing; then write down the totals at that moment and whether `error.log` stopped too.

## Part 4. T11 and T12: mod metadata and `supported_version`

**T12 (ck3-tiger), done by the maintainer before the sessions; the owner does nothing.** Re-run on 2026-10-05: `ck3-tiger --game "$GAME" mod/curia/descriptor.mod` ends with `fatal: 0, error: 0, warning: 0, untidy: 0, tips: 0` ("No problems found") after the first run's single warning (a function vanilla does not have, after a War promote) was fixed by removing that probe; the tool shows its "made for 1.19.0, 1.20.0.3 detected" banner. Planted breaks (7) in a scratch copy were all flagged. The tool does not check that every `debug_log` key exists in the localization file; a separate script did, and found every key checked by the maintainer present. `python3 tools/ci/lint_mod.py` and `bash tools/ci/scan_patterns.sh` are clean (maintainer's runs). Nothing in the game has run the mod yet.

**T11 procedure, one launch per row.** The mod must be installed with CK3 and the launcher **closed** (the installer refuses while `ck3` runs and the launcher may rewrite the folder). Part 4 needs the 0.1 block and helper functions in tab 2 (the Session 2 preflight covers it). Row A was done in Session 1 (0.6, 0.7, 2.3). For each row B to G, in this order:

1. Quit CK3 **and** the launcher. Install the row's variant (tab 2; command in the table). Then `t11_snap <row>-before`.
2. Open the launcher. Look at the mod list of the playset `curia-m3`: is "Curia" listed, enabled or disabled, is there any warning or "outdated" mark (exact text), did the playset need the mod added again, did the launcher need a restart to see it. Then `t11_snap <row>-after` and `diff ~/curia_m3_results/t11-<row>-before.sha.txt ~/curia_m3_results/t11-<row>-after.sha.txt` (empty output = the launcher changed nothing; a difference = it changed or removed something: say what, with `ls -l "$MODDIR" "$MODDIR/curia"`).
3. Play (normal, debug mode off). Does the game start without a warning of its own about the mod (exact text if one appears)? At the main menu run `t11_check <row>-menu` (tab 2).
4. Start a **new game** (any bookmark, Ironman off, name `curia_m3_t11_<row>`; the game-start line is expected at a new game, whether a loaded save prints it is not known), wait about 15 s, run `t11_check <row>-game`. **Mod-loaded evidence = the `LOAD` count above is 1 after the new game** (the second evidence is the "other lines mentioning curia" count, a count of lines, not their text, compared with the `-menu` count; the M0 stub's load showed up as mount lines). If the launcher refused the mod or the game started without it, skip the new game and record that.
5. Quit CK3. Next row.

| Row | Install command (CK3 and launcher closed) | The question it answers |
|---|---|---|
| A | `tools/m3/install_mod.sh` (Session 1, 0.6; the observations are made in 0.7 and 2.3, launch 2) | the default: `descriptor.mod` and `metadata.json` both present, `supported_version` `1.20.*` |
| B | `tools/m3/install_mod.sh --variant descriptor` | `descriptor.mod` only inside the folder, no `.metadata` folder, outer file present: listed, loaded? |
| C | `tools/m3/install_mod.sh --variant metadata` | `.metadata/metadata.json` only inside the folder, no `descriptor.mod`, outer file present: listed, loaded? |
| D | `tools/m3/install_mod.sh --supported-version '1.19.*'` | both files, `supported_version` 1.19.* on game 1.20.0.3: outdated warning? loaded? |
| E | `tools/m3/install_mod.sh --supported-version '1.18.*'` | both files, `supported_version` 1.18.*: same questions, one more version back |
| **F (required)** | `tools/m3/install_mod.sh --variant metadata && rm "$MODDIR/curia.mod"` | the folder with `metadata.json` only and **no outer `curia.mod`**: does the launcher find a mod that has no outer file at all? |
| **G (required)** | `tools/m3/install_mod.sh --variant descriptor && rm "$MODDIR/curia.mod"` | the folder with `descriptor.mod` only and **no outer `curia.mod`**: the counterpart of F |

Reading the table: the installer writes the outer `curia.mod` (the game's mod-list entry: the repository descriptor plus `path=`) in **every** variant, so rows B and C vary only the files **inside** the folder and **do not isolate descriptor versus metadata**; F and G, the only rows without the outer file, do, and "which of `descriptor.mod` / `metadata.json` the launcher needs" is read from F against G (B and C support it). Quote `'1.19.*'` (the shell would try to expand the star). The row A value `1.20.*` is also what `--supported-version '1.20.*'` would write, so it is not a separate row. `metadata.json` was written from the launcher's own validation schema (the optional `game_id` is left out): row C is the first time the launcher reads it. After row G, `tools/m3/install_mod.sh` (default) leaves the mod ready for later, or go to the final cleanup.

## Recording sheet
Fill during or right after the session (words and numbers, no game text).

**R0 State before**
| Item | Value |
|---|---|
| macOS / CK3 / Python versions | |
| Displays; separate Spaces; Stage Manager | |
| CK3 display mode: read at launch 1 main menu / confirmed at launch 2 main menu | Fullscreen |
| Launcher after install: "Curia" listed / enabled / warning text; folder still intact (0.7) | |
| Debug-mode setting in the launcher: off before launch 2 (yes/no); off at the start of Session 2 (yes/no) | |
| Achievements line at the setup screen of launch 2; at the setup screen of launch 3 (Session 2 baseline) | |

**R1 T10** (launch 1)
| Item | Value |
|---|---|
| Debug-mode setting: where, what it is called; turned off again (step 7) | |
| Console key and reply texts; spelling that worked for the data-types dump | |
| Time each command took, any freeze | |
| New files (folder, file names, sizes, times; not on the noise list of step 4) | |
| Existed the folder before | |
| Copied to `reference/ck3/` (names, sizes); `git check-ignore` printed | |
| Needed `-debug_mode` | in debug mode only / also tested without (step 7 of Part 5) |

**R2 T5, launch of launch 2**
| Item | Value |
|---|---|
| `debug.log`: event kind, old size, new size, time (watcher) | |
| `debug.log`: inode and size before, after (stat) | |
| `error.log`: event kind, sizes | |
| UTF-8 BOM at the start of `debug.log` / `error.log` | |
| Watcher output for `attached` / `waiting` / `appeared` / `disappeared` | |

**R3 Realm and T7** (2.3 and Part 5)
| Item | Value |
|---|---|
| Character, title, start date, vassals, heir (landed?), wars, claims, factions | |
| Buttons visible, positions, overlap, hover, tooltip text | |
| Click accepted paused / unpaused | |
| BEGIN name equals the ruler; date equals the HUD; `FIN` gold equals HUD gold | |

**R4 T3 latency** (summary section 2; the tool's own numbers, lower and upper bound in seconds)
| Label | Clicks made (Advisor / probes) | Frames seen (Advisor / probes) | Clicks with no line within 60 s | Lines (samples) | Lower bound min / median / max | Upper bound min / median / max | Lines needing more than one read | Longest frame span | Notes (stutter, popup, route used for `menu`, game kept running with Terminal in front) |
|---|---|---|---|---|---|---|---|---|---|
| `paused` | | | | | | | | | |
| `menu` | | | | | | | | | |
| `after_unpause` | | | | | | | | | |
| `normal` | | | | | | | | | |
| `fast` | | | | | | | | | |
| `(all labels)` | | | | | | | | | |

Also: the LOAD line's label and bounds; lines arriving together or spread (frame span); the clock section (summary section 8: is an offset between the engine clock and the Mac's clock apparent); for any condition with a `!` note about clicks with no line: the note's text, whether lines arrived late and under which label, and the attribution used.

**R5 T4 line format**
| Item | Value |
|---|---|
| Engine prefix shapes (level and source, count); prefix bytes min / median / max | |
| Line length bytes min / median / max; any line over 4000 bytes | |
| Field counts per kind equal the README table (2.5) | |
| END count equals all lines or body lines only (frames per convention) | |
| Fields that print empty or 0, per kind (FIN, HEIR, VASSAL, WAR, CLAIM, PROBE names) | |
| Date form (spaces, commas), war-name markup | |
| Non-ASCII: names found; how encoded (`xxd`); first-name versus title-prefixed names | |
| Interleaving, partial frames, duplicates, CRLF, invalid UTF-8 (summary sections 3, 4, 9) | |
| Probe groups that gave values; faction group: values or `none` | |

**R6 T8 bytes**
| Item | Advisor | Probes |
|---|---|---|
| Lines per export min / median / max | | |
| Text bytes per export min / median / max | | |
| File bytes per export (with prefix) min / median / max | | |

| Stretch | Elapsed min | In-game days | Clicks | `debug.log` total / CURIA part | `error.log` total | Per hour as printed |
|---|---|---|---|---|---|---|
| Launch to end of the Part 2 protocol (start-up, new game, clicks) | | | | | | as printed |
| 60 min with clicks (speed 3) | | | | | | as printed |
| 15 min baseline (speed 3) | | | | | | as printed |
| Optional second new game (2.9) | | | | | | as printed |
Cap (C3): **deferred** (state, reason in 3.6).

**R7 T11**
| Row | Listed / enabled | Warning text | Playset needed re-adding / launcher restart | Folder changed by the launcher (`diff`) | Game's own warning | LOAD count after the new game | Other curia lines (count) | error.log `curia[_/]` count | Verdict (loaded / not loaded / refused) |
|---|---|---|---|---|---|---|---|---|---|
| A both, 1.20.* | | | | | | | | | |
| B descriptor only (outer file present) | | | | | | | | | |
| C metadata only (outer file present) | | | | | | | | | |
| D both, 1.19.* | | | | | | | | | |
| E both, 1.18.* | | | | | | | | | |
| F metadata only, no outer file | | | | | | | | | |
| G descriptor only, no outer file | | | | | | | | | |

T12 (maintainer): as reported above.

**R8 error.log and achievements** (`errcount`; indicator read at the setup screen / pause menu / Game Rules)
| Moment | error.log `curia[_/]` count | Indicator: setup / pause menu / Game Rules |
|---|---|---|
| Start of the click protocol (2.4) | | |
| After the protocol (2.8) | | |
| Start of Part 3 (Session 2) | | |
| End of Part 3 | | |
| Notes: saving and loading with the mod (Part 5 step 6) | | |

## What to send back at the end of Session 2
What was not sent at the end of Session 1: the Session 2 cells of R0, the table R6 (stretches), R7 rows B to G, R8 rows 3 and 4, and the Part 3 and Part 4 files in `~/curia_m3_results/`; anything written down verbatim under the stop rules. Same rules as for Session 1.

## Final cleanup (after the results are in; CK3 and the launcher closed)
Tab 2:
```sh
tools/m3/install_mod.sh --uninstall
ls -l "$MODDIR"
```
The listing should show only the three workshop `.mod` files again (M0b state). In the launcher: confirm the **debug-mode setting is off**, switch back to the usual playset, delete the `curia-m3` playset (or keep it if M4 follows soon, with the usual playset active), confirm the achievements indicator reads "Available" at a plain Play's new-game setup screen. Delete the throwaway games (`curia_m3_t10_throwaway`, `curia_m3_throwaway`, `curia_m3_t8_throwaway`, `curia_m3_t11_*`) in Load Game. If Continue no longer shows the real campaign: `cp ~/curia_m3_backup/last_save.ck3 ~/curia_m3_backup/continue_game.json "$CK3/"`. `caffeinate` ends by itself (`killall caffeinate` if it still runs). Keep `~/curia_m3_results/` and `reference/ck3/` (gitignored); keep `~/curia_m3_backup/` until the Continue pointer is confirmed good.

## What happens with the results
Not part of the sessions; Phase 2 work on the maintainer's side, in this order: answers copied into `research-notes.md` §5.1 (each with date, game version 1.20.0.3 and macOS 26.5.1; T3, T4, T5, T7, T8, T10, T11, T12 marked answered except the parts listed under "Not in this session", which stay open; C3 marked deferred) and `milestones.md` M3 (status); D2 confirmed or revised from the latency data (acceptance criterion 3); the wire format frozen as `CURIA1` in `docs/wire-format.md` (field list, END convention, charset and escaping rules, size limits, the log-budget threshold default from T8; macOS-verified only); `tests/fixtures/` made by hand from the marker lines with the engine prefix removed; the frame parser with unit tests and a fuzz harness; `tools/fake_ck3` reproducing the captured structure for the CI end-to-end test; the mod's sections and probes trimmed to what the captures proved. Windows and Linux log behaviour goes to the tester checklist.
