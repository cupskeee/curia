# Milestone 0b checklist (owner, macOS, one launch, about 15 minutes)

Second M0 round. The `curia-m0` playset should still exist in the launcher, but the stub files are **no longer in your mod folder** (a listing shows only the three workshop `.mod` files), so step 1 reinstalls everything. Cleanup is **not** part of this checklist: do it after the results are in.

## What it answers
Which value forms the real export can use, from the ruler scope and from each iterated scope: gold, variables and script values via `THIS`; one probe each from **heir, vassals, factions, wars, claims**; ways to reach the **ruler** from a vassal or heir (their opinion of the ruler: through the `liege` link, a saved scope inside a script value, and a value precomputed in script); and the **maximum line length** (three long lines of 500, 2000 and 8000 characters).

## Steps
1. **Quit CK3 completely.** The stub is **not currently installed** (the launcher cleared the mod folder), so do the full install, including the outer `.mod` file, and check it is there:
   ```sh
   REPO="$PWD"   # run these commands from your curia checkout; MODDIR="$HOME/Documents/Paradox Interactive/Crusader Kings III/mod"
   rm -rf "$MODDIR/curia_m0" && mkdir -p "$MODDIR/curia_m0" && cp -R "$REPO/mod/curia-m0/." "$MODDIR/curia_m0/"
   printf 'version="0.0.1"\ntags={\n\t"Utilities"\n}\nname="Curia M0 Stub"\nsupported_version="1.20.*"\npath="%s"\n' "$MODDIR/curia_m0" > "$MODDIR/curia_m0.mod"
   ls -l "$MODDIR/curia_m0.mod" "$MODDIR/curia_m0/descriptor.mod" "$MODDIR/curia_m0/common/on_action/"
   ```
   The `ls` must show the `.mod` file, the descriptor and **two** files in `on_action` (`curia_m0_on_actions.txt`, `curia_m0_longlines.txt`).
2. Open the Paradox Launcher. Confirm the `curia-m0` playset is active and "Curia M0 Stub" is listed as **enabled** (if it is missing or greyed out, re-add it; do not launch yet). Launch with the normal **Play** button (not debug mode).
3. **New game**, Ironman off, single player. **Pick a ruler (count or higher) who has vassals and is already at war at the bookmark** (the war probes only produce data then); a ruler with a child/heir is also good. Factions and claims are optional. Name the save `curia_m0b_throwaway`.
4. **Write down** (so empty probes can be explained): character and title, start date, number of vassals (Vassals window), heir name and whether the heir holds land, whether the ruler has claims, wars, a faction against him.
5. Unpause at top speed (Space unpauses, key 5 is top speed). The first pulse comes about **11 in-game days** after the start; wait about 15 seconds of real time after the HUD date passes that point.
6. Run the helper (the game can stay open):
   ```sh
   $REPO/tools/m0/check.sh M0B
   ```
7. Optional, if you can spare 2 minutes: let about 3 more in-game months pass and run it again as `M0B2`.
8. Quit the game. (CK3 rewrites `debug.log` at the next launch; the helper already saved copies under `~/curia_m0_results/`, including `raw-*.txt` with the full lines.)

## What to record
- The helper outputs (`M0B`, and `M0B2` if done) and the facts from step 4.
- Anything odd in the game (crash, stutter, error popup).

## How to read the output
`R1`..`R8` = ruler scope; `H1`..`H7` = heir (an unlanded heir probably has no `liege`, so the court-owner variants `H5`/`H6` matter); `V1`..`V8` = the two top vassals (by military strength); `F1`..`F3` = factions; `W1`..`W3` = wars; `C1` = a random claim; `L1`..`L3` = line length (`end_tag=yes` means the whole line survived, section 2b of the output shows each length). `H0`/`V0`/`F0`/`W0`/`C0` mean "the ruler has none", not a failure. Values that print empty or `0` mean that form does not work there.
