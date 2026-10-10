<!-- SPDX-License-Identifier: MIT -->
# M3 session 2: Ironman, claims, factions (owner, macOS, about 20 minutes)

Rules as in `m3-quickrun-session1.md`: throwaway games only; words and numbers only, never game text; anything that differs from this page is the result, write it down. Two Terminal tabs, both `cd ~/PycharmProjects/curia`; tab 1 only runs the watcher, tab 2 also runs `source tools/m3/session_env.sh`. One watcher run per game or load (Ctrl+C at the end of each): the game's snapshot counter restarts at 1 in a new game, and the watcher flags a repeated number as `duplicate_snap_id`. Check tab 1 after each group of clicks.

**Stop rules.** `errcount` counts error.log lines since the game was launched, so write its value at each check and compare with the previous one; only a rise is a new Curia error. Run it after the first **Advisor** click and again after the first **Curia probes** click. On a rise: `errlines`, stop clicking, write every line (cut to 260 characters, kept local). Also stop and write the exact text on a crash, a popup after a click, a permission prompt (before answering it), or an achievements line that changes in a plain Play. An empty field or a 0 is a result, not a stop.

## Before (CK3 and the launcher closed)
1. After the probes PR is merged, tab 2: `git switch main && git pull --ff-only && tools/m3/install_mod.sh`, then `grep -c claim_detail "$MODDIR/curia/common/scripted_effects/curia_probes.txt" "$MODDIR/curia/localization/english/curia_l_english.yml"` (both counts above 0, else stop). In the launcher check that playset `curia-m3` is active with Curia on; start with the plain Play button. *Write:* launcher warnings, if any.

## Part 1: Ironman (about 4 min)
2. Tab 1: `python3 tools/m3/logwatch.py --label ironman`. New game as a small independent ruler (like the small realm of the 2026-10-10 re-check: a petty king with about 1 vassal, no war, 1 claim, an heir; write the start you used), **Ironman ON**, start, pause.
3. Click **Advisor once**, tab 2 `errcount`; click **Curia probes once**, `errcount` again. Read the achievements indicator (pause menu, second icon by Game Rules). *Write:* both buttons visible and clickable (yes/no, where); any popup (exact text); the indicator text; frames per button in tab 1.

## Part 2: claims (about 6 min)
4. Same Ironman game. Open the ruler's character window. *Write:* how many claims it lists; whether it marks them pressed, unpressed or implicit; whether the ruler's parent is alive and holds titles (yes/no, tier words only). Tab 2:
   ```sh
   RAW=$(ls -t ~/curia_m3_results/raw-curia-*.txt | head -1)
   LC_ALL=C sed 's/^.*CURIA1|/CURIA1|/' "$RAW" | grep -a -E 'PROBE\|(ruler_claim|heir_claim|claim_detail)' | cut -c1-200
   ```
   *Write:* all counts, and per `claim_detail` line the four flags (explicit-strong, explicit-weak, implicit-strong, implicit-weak) and the tier word (not the title).
5. Tab 1: Ctrl+C, `python3 tools/m3/logwatch.py --label claims_king`. Quit to the menu, load `curia_m3_throwaway` (a king with 1 claim shown in the UI), pause, click **Curia probes once**, repeat step 4 from the character window on, then `errcount`.
   Questions asked, nothing more: which filter pair gives the number the character window lists; whether the claim the UI hides in the small realm is implicit (which flag is 1); whether `ruler_claim_count` equals one of the pairs.

## Part 3: factions (about 10 min)
No faction has ever been seen in a test; do not estimate how long one takes. If none appears, that is the result.
6. **Path A (at most 5 min).** Tab 1: Ctrl+C, `python3 tools/m3/logwatch.py --label factions_a`. Quit to the menu, new game, Ironman off, as a vassal with a liege (any duke or count). Open the game's Factions window. *Write:* where it is; whether it offers to create or join a faction. If yes, create or join any one: *write* which kind it is (independence, liberty, claimant, populist, peasant, other) and the requirements it shows. Pause, click **Curia probes once** (prints `PROBE|faction_source|joined`). Nothing on offer: write that, go to path B.
7. **Path B (only if A offered nothing; at most 5 real minutes).** Tab 1: Ctrl+C, `python3 tools/m3/logwatch.py --label factions_b`. Load `curia_m3_throwaway` (or any independent ruler), run at speed 5, watching the Factions window for a faction against the ruler. At the first one: pause, click **Curia probes once** (prints `PROBE|faction_source|targeting`). *Write:* minutes run and the game date range. If none appeared: `no faction seen in N minutes`, an acceptable result.
8. With a faction, tab 2:
   ```sh
   RAW=$(ls -t ~/curia_m3_results/raw-curia-*.txt | head -1)
   LC_ALL=C sed 's/^.*CURIA1|/CURIA1|/' "$RAW" | grep -a -E 'PROBE\|faction' | cut -c1-200
   errcount
   ```
   Per faction the lines are: name, type code (1 independence, 2 liberty, 3 claimant, 4 populist, 5 peasant, 6 nomadic, 7 other), power (two forms), power threshold, discontent (two forms), member count, at war, can press demands, dangerous (1/0), leader, target. *Write:* which are empty or 0, and what the Factions window shows at that moment for kind, power, discontent and members. With 2 or more factions the watcher may list `duplicate_line` anomalies for identical per-faction lines; that is expected noise.

Deferred, recorded as open (do not test): observer mode, loading screens, the pause-menu T3 state, OS file-change notifications.

## End
9. Tab 1: Ctrl+C. Tab 2, **before the next launch of CK3**:
   ```sh
   errcount
   cp "$LOGS/error.log" ~/curia_m3_results/error-session2.log
   cp "$LOGS/debug.log" ~/curia_m3_results/debug-session2.log
   for J in $(ls -tr ~/curia_m3_results/watch-*.jsonl | tail -4); do python3 tools/m3/frame_table.py "$J"; done
   ```
   (`tail -4` = the runs made; set it to your number.) A rise in `errcount`: `errlines` first.
10. **Send back, privately** (the files hold the game's own log text; not committed or posted): the newest `summary-*`, `watch-*` and `raw-curia-*` of each run, the two copies, your notes.
11. **Cleanup** (CK3 and launcher closed): delete the Ironman and throwaway saves if wanted. Keep the mod installed until the wire-format spec is approved; remove it later with `tools/m3/install_mod.sh --uninstall`.
