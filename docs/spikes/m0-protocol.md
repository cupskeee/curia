# Milestone 0 test protocol (owner, macOS, about 45 minutes)

Goal: answer **T0, T1, T2, T6** (`research-notes.md` §5.0) with the throwaway stub mod `mod/curia-m0/`. No app, no C++. If an answer is bad we stop and re-plan (D9). Nothing is estimated: you read real results and send them back.

**Safety rules**
- Use a **throwaway** new game and a **separate playset** (your current playset has a cheat mod that also hooks game start).
- Never run this on your real campaign. Use **Load Game**, not Continue, for your real campaign until step 0 cleanup is done.
- Do not earn an achievement on purpose. Do not open the console except in run D.

## 0. Setup (once)

**Back up your saves first**, because the throwaway runs share your save folder and the Continue pointer:
```sh
CK3="$HOME/Documents/Paradox Interactive/Crusader Kings III"
mkdir -p "$HOME/curia_m0_backup" && cp -R "$CK3/save games" "$CK3/last_save.ck3" "$CK3/continue_game.json" "$HOME/curia_m0_backup/"
```
Install the stub mod (safe to re-run; keep using the same terminal window so the variables stay set):
```sh
REPO="$PWD"   # run these commands from your curia checkout
MODDIR="$CK3/mod"
rm -rf "$MODDIR/curia_m0" && mkdir -p "$MODDIR/curia_m0" && cp -R "$REPO/mod/curia-m0/." "$MODDIR/curia_m0/"
printf 'version="0.0.1"\ntags={\n\t"Utilities"\n}\nname="Curia M0 Stub"\nsupported_version="1.20.*"\npath="%s"\n' "$MODDIR/curia_m0" > "$MODDIR/curia_m0.mod"
```
Then in the Paradox Launcher: **create a new, empty playset** (e.g. `curia-m0`), add "Curia M0 Stub" to it, make it active. *Unknown until you try it:* what the launcher's add-mod screen is called and whether it finds the `.mod` file by itself. If it is missing, restart the launcher; if still missing, note what you see (do not guess). **Before every launch check that the playset shows "Curia M0 Stub" enabled.**

## 1. The check script

After each game session (while the game is still open, or right after quitting, **before the next launch**; CK3 appears to rewrite `debug.log` at every launch, seen once), run:
```sh
$REPO/tools/m0/check.sh A      # use the run letter from the table below
```
It prints, and saves under `~/curia_m0_results/`: whether the mod loaded, every `CURIA_M0` test line with the line after it, whether test lines show up in any other log, errors mentioning the mod, and byte sizes. **If section 1 says the mod did not load, the run is invalid (not a T1 failure): fix the playset and repeat.** Lines can take up to about 3 s to appear (the game's log settings say 3 s; not yet observed for `debug_log`), so wait about 10 s before running it.

## 2. Runs (two launches)

Pick a **ruler** (count or higher, not an unlanded character) in every new game. Space pauses; key 5 is top speed; if nothing moves, the game is paused.

| Run | Launch | Steps | Record |
|---|---|---|---|
| **A. Normal (T0, T1, T2, T6)** | Play (not debug mode) | New game, any bookmark, single player, default rules, **Ironman off**. On the setup screen read the **Achievements line right of the Ironman Mode checkbox** (baseline). Name the save `curia_m0_throwaway`. Start. Wait ~10 s, run the check script as `A`. Press Esc: the **second (lower) icon beside Game Rules** is achievements (the first is Ironman); hover it; open Game Rules and read its Achievements line. Unpause at top speed for about 3 in-game months until a `Q` line appears; run the script again as `A2`. Save, quit to the main menu, open **Load Game**, click the throwaway save and read the **detail pane** ("Achievements Allowed / Disabled"; a list row shows an icon only when disabled). | Script output; each icon/tooltip text exactly as shown; in-game days from start until the first `Q1` line (read the date on the HUD). |
| **B. Ironman (T1)** | same session as A | From the main menu start another new game with **Ironman on**. Wait ~10 s, script as `B`. Esc: read the icons. Save name `curia_m0_ironman`. | Do `S` lines appear? Icon text. |
| **C. Debug-mode launch (T1 fallback, T2)** | Quit the game fully, then the launcher's alternative entry "Open game in Debug Mode" (*where it is: note what you see*) | New game, Ironman off, name `curia_m0_debug`. **Use menus only: do not open the console or click any debug button.** Wait ~10 s, script as `C`. Esc: is the achievements icon still "Available", with a small extra warning mark? | Do `S` lines appear? Icon and warning text. |
| **D. One console command (T2c; throwaway game only; needed for the T2 record, skip only if you note it in the results)** | same session as C | Open the console (the key is not documented in these notes: if you can't find it, say so in the results; the wiki says it is reachable in debug mode), run **only** `help`. Watch the Esc-menu icon: immediately, after the next save, or never? | When it changed. Expected "Not Available". **Never load this save with your real playset; delete it afterwards.** |

After run D (or C), quit and launch once with plain **Play** and check no debug warning mark appears.

## 3. Reading the test lines

Each line is `CURIA_M0|<id>|<label>|<text>` after the engine's own prefix. `S1`..`S8` come from the game-start hook (root none, player reached through a saved scope), `Q1`..`Q8` from the quarterly hook (root = your ruler). Write the **run letter** next to every output you send.

| What you see | Meaning |
|---|---|
| `name=Some Character`, `gold=123`, `const=1234`, `var=42` | resolved: that form works |
| the literal bracket text exactly as in the stub (e.g. `[SCOPE.sC('curia_m0_player').GetName]`, `[ROOT.Char.GetName]`) | brackets are **not** resolved in that form |
| a line missing, or only the key name (`curia_m0_s3_name`) | key not found or the line failed; read the error.log part of the script output |
| `1234.000`, colour or markup characters around values | note the exact text (that is the number/markup format) |

`S1`/`Q1` plain (T1); `S2`/`Q2` quoted string with brackets (T6a); `S3`..`S8`/`Q3`..`Q8` localization keys (T6b/T6c). `S8`/`Q8` use the exact form vanilla already uses with `debug_log`, so if only `S8`/`Q8` resolve, the quoted and `sC`/`ROOT` forms are the problem, not brackets in general. If the `Q` forms resolve but the `S` forms don't, saved scopes are not visible to `debug_log`.

## 4. Results to record (copy into `docs/spikes/m0-results.md`)

Header: date, game version (launcher shows 1.20.0.3), macOS version (the script prints both).

| ID | Question | Answer |
|---|---|---|
| T0 | `debug.log` exists at the path in the script output after launch; size after ~10 s | |
| T1 | Normal launch (A): do `S1` and `Q1` appear? Seconds after the event (from timestamps)? In which log file? | |
| T1 | Ironman (B): do `S` lines appear? (If not: **stop**, NFR-6) | |
| T1 | Debug-mode launch (C): do `S` lines appear? | |
| T2 | Icon text: setup screen / after start / after lines fire / Load Game detail pane (A, B) | |
| T2 | Debug-mode launch: icon state and warning mark text | |
| T2c | After `help` (D): did it change, and when? | |
| T6a | Quoted string with brackets (`S2`, `Q2`): resolved? | |
| T6b/c | Keys (`S3`..`S8`, `Q3`..`Q8`): which resolved; exact text of numbers | |
| size | `marker bytes` and `marker lines` from the script; bytes per line | |
| notes | `error.log` entries mentioning the mod; any odd launcher behaviour | |

## 5. What the answers decide

- **T1:** normal launch writes the lines → no `-debug_mode` needed. Only run C writes them → debug mode is required: check what that does to achievements (T2) and **stop for a decision**. Nothing writes in any run (with the mod confirmed loaded) → **stop**. Ironman run silent → **stop** (NFR-6 asks for Ironman compatibility).
- **T2:** "Available" in runs A, B and (with only the warning mark) C → mods and `debug_log` don't block achievements: compatibility becomes a design goal. "Not Available" before any console command → **stop** and record exactly when it flipped.
- **T6:** all useful forms resolve → real data can be exported. **Partial:** record which forms resolve and ask the maintainer before proceeding (the exporter would use only those; not an automatic stop). **None resolve** → **stop** (the export design depends on it).
- Any stop means no C++ until we have re-planned together (D9).

## 6. Cleanup

Quit the game; switch the launcher back to your usual playset. Delete the throwaway and Ironman saves (`curia_m0_*`) from Load Game; if Continue no longer shows your real campaign, restore from `~/curia_m0_backup`. Remove `"$MODDIR/curia_m0"` and `"$MODDIR/curia_m0.mod"`. Keep `~/curia_m0_results/` (CK3 overwrites its logs on each launch); raw logs are never committed.
