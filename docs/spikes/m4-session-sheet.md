<!-- SPDX-License-Identifier: MIT -->
# M4 game sittings (owner, macOS, in the game): Sitting A about 1 hour, Sitting B about 15 minutes, S4 during normal play; about $2

**Build from commit `<commit: filled in after the dev to main merge>`** (the merged `main`).

Purpose: the first time Curia runs over the real game. You click Advisor in Crusader Kings III, the
overlay opens, you ask your advisor and the answer streams in. Sitting A checks this with CK3 in
Fullscreen, together with the overlay's keys, the chat window's controls, a Stop, the prompt cache
and the monthly-tick test (S3). Sitting B repeats the overlay checks with CK3 Windowed. S4 measures
how fast the game's log grows during normal play over the following days. The connection check
(S0) is not repeated: it was done in the quality sitting (`m4-quality-results.md`).

Curia only reads the game's log; it writes nothing into the game folders and never uses the
console. The questions cost money (see "Cost"). The key is typed only at the hidden prompt, in the
Terminal window you start Curia from (and, for the optional section 8, in one second window).

**Rules for this sheet**
- Never paste the key anywhere: not into a chat message, not into a file, not into this sheet, not
  after `--args`. Step 1.2 uses a prompt that shows nothing and keeps nothing in the shell history.
- The app never prints or logs the key. If you ever see something that looks like a key on the
  screen or in a file, stop and tell the maintainer (where, not what).
- What you send back is the output of the two tools (numbers and fixed words) and the filled
  recording tables (words and numbers; "the heir", "a vassal", no names). The app log itself, the
  `stderr.txt` files, screenshots, the answers and your saves stay on your Mac. The app log holds
  only numbers and short words, but it stays private anyway.
- Anything that differs from this sheet is a result: write it down, do not work around it.
- A macOS permission prompt (any app): press **Don't Allow**, write its exact text and the app it
  names, then go on.

## 0. Before the sittings (once, about 15 minutes plus the build)

1. **One Terminal window**, on the display that is **not** CK3's (with one display: reach it with
   Cmd+Tab). Use it for everything below; the key lives only in this window.
2. **Build** the merged `main`:

   ```sh
   cd ~/PycharmProjects/curia
   git switch main
   git pull --ff-only
   git log -1 --format=%h
   export VCPKG_ROOT=~/vcpkg
   cmake --preset macos-arm64
   cmake --build --preset macos-arm64 --target curia_app
   ls -d build/macos-arm64/app/Curia.app
   ```

   The `git log` line must print the commit at the top of this sheet (its first characters); if
   not, stop and tell the maintainer. The last line must print the path of `Curia.app`. Do not run
   `ctest` for this sitting (nothing needs it, and the window tests stay off unless you switch them
   on, D25).
3. **The mod, version 0.2.1.** Quit CK3 and the Paradox Launcher completely, then:

   ```sh
   tools/m3/install_mod.sh --dry-run
   tools/m3/install_mod.sh
   grep '^version' "$HOME/Documents/Paradox Interactive/Crusader Kings III/mod/curia/descriptor.mod"
   ```

   The last line shows `version="0.2.1"`. The installer refuses while CK3 runs (exit 2). In the
   launcher the playset with Curia stays active and enabled, as in M3. Start the game only with the
   plain **Play** button, never the debug launch button.
4. **A spending limit.** In the service's console set a spend limit of **$5** for the key you use
   (or for its workspace). This sheet needs about $2.
5. **No Curia running:**

   ```sh
   pgrep -lx curia || echo "Curia is not running"
   ```

   If it prints a line with `curia`, quit that copy first (its menu-bar icon, "Quit Curia"; or
   `pkill -x curia`).
6. **The result folders** (private to your user):

   ```sh
   umask 077
   mkdir -p ~/curia_m4_results/sitting-a ~/curia_m4_results/sitting-b ~/curia_m4_results/s4
   chmod 700 ~/curia_m4_results
   sw_vers -productVersion
   ```

7. **Write down (R0):** the date, the macOS version (last line above), the CK3 version (launcher),
   the number of displays and which one CK3 will be on, "Displays have separate Spaces" (System
   Settings, Desktop & Dock, Mission Control; **look, do not change it**), Stage Manager on or off,
   and the commit from step 2.

## 1. Starting Curia (every sitting, about 3 minutes)

The launch method is decided (D24): the variables are exported in this shell and `Curia.app` is
started with `open -g -n`. `open` hands its own environment to the app, so the key reaches Curia
without ever being on a command line, and macOS treats Curia, not Terminal, as the app: any
permission prompt would be Curia's. Do not use `--env` (it would pass an empty value).

1. **Name the sitting** (`OUT` is the folder of this sitting's app log; the block is for Sitting A):

   ```sh
   cd ~/PycharmProjects/curia
   APP="$PWD/build/macos-arm64/app/Curia.app"
   OUT="$HOME/curia_m4_results/sitting-a"
   ```

   For Sitting B use `OUT="$HOME/curia_m4_results/sitting-b"`, for the S4 days
   `OUT="$HOME/curia_m4_results/s4"`.
2. **The key and the model** (the prompt shows nothing while you type; press Enter when done):

   ```sh
   read -s "CURIA_ANTHROPIC_API_KEY?API key (nothing is shown): "
   export CURIA_ANTHROPIC_API_KEY
   echo
   export CURIA_MODEL=claude-sonnet-5-5
   ```

3. **Start it** (quit a running copy first: `-n` would start a second one, which then cannot take
   the hot key):

   ```sh
   if pgrep -x curia >/dev/null; then echo "STOP: Curia is already running; quit it first"; else open -g -n "$APP" --stderr "$OUT/stderr.txt" --args --log-dir "$OUT/applog"; fi
   sleep 3
   pgrep -lx curia
   grep ' dev_config ' "$OUT/applog/curia.log" | tail -1
   ```

   `pgrep` prints one line with `curia`. The `grep` line ends with `dev_config key=set model=set`
   (words only: which variables arrived, never a value). `key=unset` or `model=unset`: the variable
   was not exported in this window; quit Curia, repeat step 2, start again (in S4 without the key,
   `key=unset` is expected). `key=unusable` or `model=unusable`: the value arrived but was refused
   (a space or another character that a key or a model name cannot hold, often pasted with it);
   quit Curia, type it again in step 2, start again. Every path after `--args` must be absolute
   (the app runs in `/`); `$OUT` is one. Error messages of the app go only to
   `$OUT/stderr.txt`. The app log of the sitting is `$OUT/applog/curia.log` (`--log-dir`); without
   that setting it would be `~/Library/Logs/Curia/curia.log`, where `curia --check-api` (section 8)
   writes.

   The overlay appears once at start-up (on the display with the pointer while CK3 is not running),
   so that you see that Curia started. If starting Curia made it the active app, it hands the keys
   back a quarter of a second later.
4. **Stopping Curia** (end of a sitting): its menu-bar icon, "Quit Curia". If there is no icon:
   `pkill -x curia`.
5. **If typing in the overlay fails** (Sitting A, step A4), quit Curia and start it again with one
   more setting after `--args`, in this order, each one only if the one before failed:
   - `--overlay-focus click`: you click the overlay's input box once before typing;
   - `--overlay-focus activate`: Curia makes itself the active app when the overlay opens; check
     that CK3 stays in Fullscreen;
   - `--window plain`, with CK3 in **Windowed**: a normal window next to the game (the last
     fallback, FR-OVL-8).

   For example: `open -g -n "$APP" --stderr "$OUT/stderr.txt" --args --log-dir "$OUT/applog" --overlay-focus click`.

   With CK3 running, the restarted Curia shows its start-up overlay over the game. That one never
   takes the keys (it only shows that Curia started), so do not test with it: press **Ctrl+Cmd+K**
   to hide it, then repeat A4 from step 2 (the Advisor click).

## 2. Sitting A: CK3 Fullscreen (about 1 hour, about $1)

Criteria 1, 3, 4, 7 and 10 (the table in section 6). Tick or note each step in table RA. Each
question costs a few cents. Time plan: A0 to A3 about 15 minutes (with the game's start), A4 and A5
about 10, A6 to A9 about 15, A10 (S3) about 15, A11 and A12 about 5, then section 5 (5 minutes).

Good to know: when you open the overlay with Ctrl+Cmd+K or the menu-bar icon more than 2 minutes
after the last Advisor click and do not click Advisor within 15 seconds, it says "No update from
the game yet. Click Advisor in the game; if that does nothing, restart CK3." That is the
log-silence detector (D17), not a fault; clicking Advisor clears it. Its second detector says
"Nothing new has come from the game for a while. If clicking Advisor does nothing, restart CK3."
when the game's log has been silent for 10 minutes while CK3 runs (for example while the game is
paused for long). Both are guesses to be tuned from real play: write in RA when the second one
appears and what the game was doing (paused, running, in a menu); the app log has the time.

**A0. Curia first, CK3 not yet in front (5 minutes).**
1. Start Curia (section 1, `OUT` for Sitting A). The overlay appears.
2. Look: **no Curia icon in the Dock**, none in the Cmd+Tab switcher; Curia's **menu-bar icon**
   (a red disc with a "C") is there; **no permission prompt**.
3. Press **Ctrl+Cmd+K**: the overlay hides. (The hot key works before CK3 runs.)

**A1. CK3 Fullscreen (5 minutes).** Start CK3 with plain Play, Display mode **Fullscreen** (the
game's settings, or the launcher's Game Settings). Load a game where your ruler has an heir and
some vassals (a normal campaign is fine: nothing here changes the game). Do not click the **Curia
probes** button in this sheet.

**A2. The first Advisor click.** Pause the game (**Space**), then click **Advisor**.
- The overlay appears **on CK3's display**, at the right edge of the game, and **CK3 stays in
  Fullscreen** (no switch of Spaces, no flicker to the desktop).
- The top of the overlay says what it can see and the date ("What I can see is from ...").

**A3. The pause reminder.** Above the input box: "**Pause the game (Space)** before you click
Advisor: it keeps running while you type." Does it read right, in the order you do things (pause
first with Space, then click Advisor)? Note any wording that confuses you.

**A4. Keys (E6, the most important check).**
1. Press **Esc**: the overlay hides.
2. With the game paused, click **Advisor**. That click goes to the game, so CK3 has the keys at
   that moment; the overlay opens. Do **not** click the overlay.
3. Type `a b 1 2` **without clicking** (letters, spaces and digits: Space is the game's pause key
   and the digits are its speed keys). The text appears in the overlay's input box and **the game
   does not react**: it stays paused, its speed does not change, no window opens. If keys do reach
   the game, nothing is harmed: pause again with Space.
4. Press **Esc**: the overlay hides at once. **Without clicking**, press **Space**: the game
   unpauses at once (the date moves). Press **Space** again to pause.
5. Press **Ctrl+Cmd+K**: the overlay opens and `a b 1 2` is **still in the input box**. Delete it.
6. The same with the hot key: press **Esc**, click once on an empty spot of the map (close a panel
   if one opens), press **Ctrl+Cmd+K**, type `a b 1 2` without clicking: again in the input box,
   the game stays paused and its speed does not change. Delete it. **Ctrl+Cmd+K** again hides the
   overlay (it toggles with CK3 in front).

If step 3 or 6 fails (the keys go to the game, or nowhere): write what happened, quit Curia and
follow 1.5 (`--overlay-focus click`, then `activate`, then `--window plain` with CK3 Windowed),
repeating A4 from step 2 with each (hide the start-up overlay with Ctrl+Cmd+K first, as 1.5 says).

**A5. The menu-bar icon** (two displays only; over CK3's Fullscreen display the menu bar may not
appear). Move the pointer to the **other** display's menu bar, click Curia's icon: **Show Curia**
(the overlay appears on CK3's display), then **Hide Curia**. Keep "Quit Curia" for the end.

**A6. S1: the first question (criterion 1).** Pause, click Advisor, type `is my heir safe?` and
press **Enter**.
- "thinking..." shows, then the answer **streams in word by word**.
- It cites **real values**: compare the heir it names with your game, and any gold figure with the
  top bar (write yes or no, no names). An answer about the heir may not name the gold; then check
  the gold in the answer of A7.
- The view follows the answer as it grows.

From A7 to A9 you type into the overlay several times. Whenever you have clicked somewhere else
since the last time you typed (into the game, Terminal or the menu bar), **click once into the
overlay's input box first**: the click gives the keys back to the overlay (clicking is allowed
outside A4). Otherwise what you type goes to that other window: Terminal would run it as a command,
and the game would take Space as unpause.

**A7. The second question within 5 minutes (criterion 4, the cache).** Within 5 minutes of A6 and
**without** a new Advisor click: click once into the overlay's input box (A6 may have taken you to
the game), type `what should I focus on?`, then **Shift+Enter** (a new line in the box, nothing is
sent), `answer in three lines`, then **Enter** (sent). The analysis reads whether this question
was served from the cache.

**A8. Stop (criterion 3).** Type `who might rebel? explain each vassal in detail`, Enter, and press
**Stop** as soon as the first words appear: the answer stops and says "Stopped.". Then type
`can I afford a war? explain in detail`, Enter, and press **Cmd+.** (Command and full stop) once
words appear: it stops too. If an answer finished before you stopped it, ask the same question
again and stop it sooner; write in RA how many tries it took. Optional: later, the service
console's usage page shows the output tokens of these requests; a small number supports
"generation stopped".

**A9. The chat window.**
1. **Copy**: click Copy under an answer ("Copied" shows). In Terminal: `pbpaste | wc -w` prints a
   number of words above 0 (the text itself is not shown).
2. **Hidden, still arriving**: go back to the game (with one display: Cmd+Tab to CK3) and **click
   once into the overlay's input box** (the keys are in Terminal now). Ask `explain my vassals one
   by one`, press **Esc** at once, wait 15 seconds, Ctrl+Cmd+K: the conversation is still there and
   the answer is complete (it kept arriving while hidden). If the conversation is now longer than
   the view, scroll up and down.
3. **Retry after an error** (skip if it is not practical): turn Wi-Fi off (or the network you
   use), **click once into the overlay's input box**, ask `is my realm at peace?`; an error appears
   in the answer (for example "Could not reach the service. ..."; the words may differ: write the
   first words and roughly how long it took). Turn the network on, click **Retry**: the answer
   arrives.

**A10. S3: the monthly-tick test (about 15 minutes; criterion 10).** It tests whether the income
figure the mod reads changes only at the monthly tick while the tooltip changes live.
In this test the overlay must not hold the keys while you unpause or change the speed: press
**Esc** after every Advisor click (the overlay hides and the keys go back to the game), as in S4.
If Sitting A runs with `--overlay-focus click` (A4 fallback), the overlay does not take the keys
and Esc would go to the game: press **Ctrl+Cmd+K** instead wherever this sheet says Esc after an
Advisor click.
1. Hide the overlay (**Esc**, or Ctrl+Cmd+K). Unpause until the date is near **day 27 or 28** of a
   month, pause.
2. Hover the gold in the top bar: write the **monthly balance** the tooltip shows (for example
   `+7.8`) and the date into table RS3. Click **Advisor**, then press **Esc**.
3. At the slowest speed, unpause and pause again as soon as the date has moved on by **one day**.
   Write the tooltip's balance and the date, click **Advisor**, press **Esc**.
4. Repeat step 3 until the date is **day 2 of the next month** (six to eight readings, across the
   month boundary).
   The analysis prints the income the mod sent at each click (S3 table, the series of readings one
   day apart); if the hypothesis holds, it changes only between the last reading of the old month
   and the first of the new one, while the tooltip may change on other days too. If the income did
   not change at the boundary either, the analysis says "not shown": that month cannot tell.

**A11. CK3 not running.** Quit CK3 completely (its own menu, Quit to desktop). If the overlay is
still visible, press Ctrl+Cmd+K once to hide it. Then press **Ctrl+Cmd+K** to open it and wait
20 seconds. The overlay says "**CK3 is not running.** Start it, then click Advisor in the game."
and not "No update from the game yet ... restart CK3".

**A12. The end of Sitting A (5 minutes).**
1. Curia's menu-bar icon, **Quit Curia**: the icon goes away and `pgrep -lx curia` prints nothing.
2. System Settings, Privacy & Security: Accessibility, Contacts, Input Monitoring, and Screen &
   System Audio Recording do **not** list Curia (look, do not change anything).
3. Section 5 (the two tools).

## 3. Sitting B: CK3 Windowed (about 15 minutes; criterion 7)

1. CK3: Settings, Graphics, Display mode **Windowed** (or in the launcher's Game Settings); if
   the game asks to restart, let it. Then quit CK3.
2. Start Curia (section 1 with `OUT="$HOME/curia_m4_results/sitting-b"`; if Sitting A needed a
   fallback setting, use the same one). Start CK3 (plain Play), load the game.
3. Pause, click **Advisor**: the overlay is **visible** over the game window, on CK3's display.
4. Repeat A4 (keys: type `a b 1 2` without clicking, the game stays paused at the same speed; Esc,
   Space reacts at once, the draft stays, Ctrl+Cmd+K). Click into the overlay once: it takes the
   click and the keys.
5. **Second display** (two displays only): move CK3's window to the other display, pause, click
   Advisor: the overlay follows to that display. Move it back.
6. Optional: one question, to see the answer stream in this mode.
7. Quit CK3, Quit Curia (menu-bar icon), section 5 for Sitting B. Set CK3 back to Fullscreen if
   that is how you play.

## 4. S4: bytes per hour during normal play (the following days; criterion 8)

How much the game's log grows per hour of normal play, with and without Curia's own lines. Curia
counts it by itself; you only play.

1. For each play session: start Curia **before** CK3 (section 1 with
   `OUT="$HOME/curia_m4_results/s4"`; the same folder every day, the app log grows across days). The
   key is optional here: without step 1.2 the overlay says that Curia is not set up to talk to the
   service, and everything this part needs still works.
2. As soon as the save has loaded: pause, click **Advisor**, press **Esc** (the overlay hides and
   the keys go back to the game), unpause. This first click marks the end of the launch and the
   loading, which write far more than play; the analysis counts play from it (without any click,
   from 10 minutes after the launch). Then play normally, and about every **10 minutes** the same:
   pause, click **Advisor**, press **Esc**, unpause. (With `--overlay-focus click`: Ctrl+Cmd+K
   instead of Esc, as in A10.)
3. At the end of each session: quit CK3, then Quit Curia (menu-bar icon). Fill one row of table
   RS4: the date, when you started and stopped, the **game speed** you mostly played at, about how
   many minutes the game was paused, and roughly how many Advisor clicks.
4. After two to three hours of play in total (or when you like), section 5 for S4. Sessions of any
   length count. The S4 table gives the bytes per hour, overall and per hour of play, with and
   without Curia's lines, counted only while CK3 ran, and the "Play only" figure without the launch
   and the loading. Whether a separate stretch without clicks is still needed is decided from these
   numbers (expected: no, M4 criterion 8).

## 5. After each sitting: the two tools (about 5 minutes)

In the same Terminal window (the key is still exported there; `$OUT` names the sitting). After an
S4 session without the key, do step 1.2 first: `grep_key.py` needs the key to look for it.

```sh
cd ~/PycharmProjects/curia
python3 -I tools/m4/analyze_applog.py "$OUT/applog" > "$OUT/analysis.md" 2>&1; echo "analyze exit $?" | tee -a "$OUT/analysis.md"
python3 -I tools/m4/grep_key.py ~/curia_m4_results ~/Library/Logs/Curia build/macos-arm64 ~/.zsh_history ~/.zsh_sessions > "$OUT/grep_key.txt" 2>&1; echo "grep_key exit $?" | tee -a "$OUT/grep_key.txt"
python3 -I tools/m4/grep_key.py ~/Library/Logs/DiagnosticReports --name-contains curia >> "$OUT/grep_key.txt" 2>&1; echo "grep_key exit $?" | tee -a "$OUT/grep_key.txt"
open -e "$OUT/analysis.md" "$OUT/grep_key.txt"
```

- `analyze exit 0` is normal; `1` means its last section found a line shaped like an API key: do
  not send the app log, tell the maintainer (the section names only line numbers). `2`: no app log
  could be read, or the tool stopped on an error of its own (its message says which; nothing else
  is printed): tell the maintainer.
- `grep_key exit 0`: the key was found nowhere. `1`: it lists the files that hold it, or a piece of
  it (its first 24 characters, as a cut copy would), by name only (`KEY FOUND`); do not open or
  send them, tell the maintainer the names. `2`: the key is not exported in this window (step 1.2),
  none of the paths exists, or the tool stopped on an error of its own (its message says which).
  `3`: **not complete**: the key was not found in what could be read,
  but the lines `could not read` or `partly scanned` name files or folders it could not read in
  full; tell the maintainer those names (for `partly scanned`, running the same line again with
  `--max-bytes 4000000000` after the paths reads the whole file). A path that does not exist on
  your Mac is reported and skipped (for example `~/.zsh_sessions`).
- Lines `shape sk-ant- ...` under `build/macos-arm64` name the test programs (`curia_tests`,
  `test_redact.cpp.o` and others): those are the tests' made-up keys, not yours. Only `KEY FOUND`
  and the exit code are about your key. Elsewhere a `shape sk-ant-` line without FAKE may be
  another key of yours: tell the maintainer the file name. The tool does not look inside
  compressed files (`.gz`, `.zip`).
- The two files hold only numbers, fixed words and file names; your home folder appears as `~`,
  not by your account name. **Send back both files**, plus the filled tables of section 7.
- With the S4 log, the analysis covers all days at once; send its "S4" section and the RS4 table.

Optional, a number only: how many lines of the game's `error.log` mention Curia (expected 0).
`errcount` of the M3 helpers counts them and prints the number, never a line:

```sh
source tools/m3/session_env.sh && errcount
```

Write the number in RA (row "error.log").

Then close the window or forget the key:

```sh
unset CURIA_ANTHROPIC_API_KEY CURIA_MODEL
```

## 6. Which criterion is checked where

| M4 criterion | Step | Evidence (what the maintainer reads) |
|---|---|---|
| 1 Click Advisor, overlay opens within the M3 latency, the answer streams and cites real values | A2, A6 | Readings table: "Overlay shown after (ms)" (the app's part, from the reading's arrival; the part before is M3's flush latency, no line more than about 1 s after its engine second, plus up to 250 ms for the app's poll); RA rows A2 and A6 (streams, gold and heir right); Questions table (first token, total) |
| 3 Cancel stops generation, the connection is closed (assumption A11 verified or documented) | A8 | Stops table: Stop to request end in ms, "cancelled", connection "closed" (the client's classification: libcurl closes such a connection, per its source and the http tests in CI; not observed on the socket here), HTTP 1.1, the next request's new connections and the seconds before it, compared with a question after a finished answer (Questions table); optional: the console's output tokens |
| 4 A second question in the same reading reads the cache | A7 | Cache table: cache_read > 0, or the reason in the numbers (cache write, seconds apart) |
| 6 No API key in logs or crash output | section 5 | grep_key.txt (exit 0 for each run; 3 is not complete), the Key check section of analysis.md |
| 7 Overlay over Fullscreen and Windowed, keys, no Dock icon, tray at start-up, CK3's display, the in-game button | A0, A2 to A5, A12, Sitting B | RA and RB rows; Overlay tables: overlay_placed basis `game`, overlay_shown and overlay_focus (`first_key` with the panel key and Curia not active), overlay_keys; Runs table: tray `created`, hot key `registered`, activation policy `accessory` |
| (5 is CI; the real game here) The log-silence notices (D17): "CK3 is not running", not "restart CK3" | A11, any | RA rows A11 and "Nothing new"; CK3 and notices table (`ck3_running false`, `Ck3NotRunning`) |
| 8 Bytes per hour of normal play, with and without Curia's lines | S4 | S4 tables ("Play only": without the launch and the loading) and RS4 (game speed, paused time) |
| 10 Monthly-tick test | A10 | S3 table (the mod's income per reading; the verdict of the series of A10, the readings one day apart) beside RS3 (the tooltip per reading) |

Criteria 2 and 5 are checked by CI; criterion 9 was the quality sitting (done).

## 7. Recording tables (words and numbers only; no names)

**R0: setup**

| Item | Value |
|---|---|
| Date | |
| macOS version | |
| CK3 version | |
| Mod version installed | 0.2.1 (from step 0.3) |
| Build commit | |
| Displays (how many, which one is CK3's) | |
| Displays have separate Spaces | |
| Stage Manager | |

**RA: Sitting A (Fullscreen)**

| Step | Check | Result (yes / no / what happened) |
|---|---|---|
| A0.2 | no Dock icon, not in Cmd+Tab; menu-bar icon present; no permission prompt | |
| A0.3 | Ctrl+Cmd+K hides the start-up overlay | |
| A2 | overlay on CK3's display; CK3 stays in Fullscreen | |
| A3 | pause reminder reads right (pause with Space, then click Advisor) | |
| A4.3 | after an Advisor click, typed `a b 1 2` without clicking: in the input box; the game stays paused, same speed | |
| A4.4 | Esc hides; Space reaches the game at once, no click | |
| A4.5 | the draft is still there | |
| A4.6 | the same after a click on the map and Ctrl+Cmd+K; Ctrl+Cmd+K toggles with CK3 in front | |
| A4 fallback | which setting was needed (none / click / activate / plain) | |
| A5 | menu-bar icon from the other display: Show, Hide | |
| A6 | thinking..., then word by word; heir right; gold right (A6 or A7); view follows | |
| A7 | second question sent within 5 minutes, Shift+Enter made a new line | |
| A8 | Stop stops ("Stopped."); Cmd+. stops; tries needed (1 if the first stop came in time) | |
| A9.1 | Copy: `pbpaste \| wc -w` above 0 | |
| A9.2 | answer complete after hiding; conversation kept; scrolling | |
| A9.3 | the error's first words and how long it took; Retry worked (or skipped) | |
| A11 | "CK3 is not running", not "restart CK3" | |
| A12 | Quit Curia from the icon; no Curia entry in Privacy & Security | |
| any | "Nothing new has come from the game for a while" seen? when, and what the game was doing | |
| error.log | lines mentioning Curia (`errcount`, section 5; optional) | |
| any | permission prompts (exact text, app named) | |
| any | anything else that surprised you | |

**RS3: the monthly-tick test**

| Reading | Game date (top bar) | Tooltip monthly balance (as shown) |
|---|---|---|
| 1 | | |
| 2 | | |
| 3 | | |
| 4 | | |
| 5 | | |
| 6 | | |
| 7 | | |
| 8 | | |
| 9 | | |

**RB: Sitting B (Windowed)**

| Step | Check | Result |
|---|---|---|
| 3 | overlay visible over the game window, on CK3's display | |
| 4 | typing without a click reaches the overlay, not the game | |
| 4 | Esc gives the keys back (Space reacts at once); the draft stays | |
| 4 | a click into the overlay takes the keys | |
| 4 | Ctrl+Cmd+K toggles | |
| 5 | the overlay follows CK3 to the other display | |
| 6 | (optional) answer streams | |

**RS4: S4 play sessions**

| Date | From | To | Game speed (mostly) | Paused (about, minutes) | Advisor clicks (about) | Notes |
|---|---|---|---|---|---|---|
| | | | | | | |
| | | | | | | |
| | | | | | | |

## 8. Optional, free: a wrong key and a wrong model

Each sends one request that the service refuses before any model runs. In a **second** Terminal
window (so that the real key of the sitting window is not touched):

```sh
cd ~/PycharmProjects/curia
export CURIA_ANTHROPIC_API_KEY=sk-ant-FAKE-wrong-key-for-the-401-test-0000
export CURIA_MODEL=claude-sonnet-5-5
build/macos-arm64/app/curia --check-api
```

Expected: a line `[FAILED] Sign-in header ...: rejected (HTTP 401)` and a `CHECK_API_RESULT` line
with `status=401`. **Never type the real key into this test**: the line above is a made-up key
that says FAKE. Then the wrong model, with the real key at the hidden prompt:

```sh
read -s "CURIA_ANTHROPIC_API_KEY?API key (nothing is shown): "
export CURIA_ANTHROPIC_API_KEY
echo
export CURIA_MODEL=claude-no-such-model-0
build/macos-arm64/app/curia --check-api
unset CURIA_ANTHROPIC_API_KEY CURIA_MODEL
```

Expected: `model was not found` (HTTP 404). **Write down** both `CHECK_API_RESULT` lines (they hold
no secret) and close this window.

## Cost

About **$2** in total: each question sends a few thousand tokens of prompt (cheaper when it comes
from the cache) and receives a few hundred; Sitting A asks about eight questions, Sitting B one,
S4 none unless you ask. The $5 limit of step 0.4 is the safety net. The usage page of your account
shows the exact figures; you may send the total.

## Troubleshooting

| What you see | What it means and what to do |
|---|---|
| `dev_config key=unset` or `model=unset` (step 1.3) | The variable was not exported in this window. Quit Curia, step 1.2 again, start again. In S4 without the key, `key=unset` is expected. |
| `dev_config key=unusable` or `model=unusable` (step 1.3) | The value arrived but was refused (a space or another character a key or a model name cannot hold). Quit Curia, type it again in step 1.2, start again. |
| `STOP: Curia is already running` | Quit the running copy (menu-bar icon, or `pkill -x curia`), then start again. |
| Nothing appears after `open`, `pgrep` prints nothing | Read `cat "$OUT/stderr.txt"` (the app's own messages; no secret) and tell the maintainer what it says. |
| The overlay says Curia is not set up to talk to the service | The key or the model did not arrive (see the first row). In S4 without the key this is expected. |
| "Curia can't find CK3's folder in Documents ..." | Start CK3 once (it creates its folder), quit it, restart Curia. |
| The overlay shows nothing about the realm after a click | Was the game paused and the click on **Advisor** (not Curia probes)? Click again. Then tell the maintainer, with the time. |
| "The last update came from the test button (Curia probes)" | The probes button was clicked; click Advisor instead. |
| "The Curia mod in the game and this app are different versions" | Step 0.3 again with CK3 and the launcher closed. |
| An error in the answer ("Could not reach the service", "The service is busy right now", "too many requests") | Retry once; if it repeats, write the time and the words. |
| The hot key does nothing | Another Curia may hold it: the Runs table of the analysis shows the hot key as `taken`. Quit every Curia and start one. |
| `zsh: no matches found` or odd quotes | A line was copied with other quote characters. Copy it again from this page. |
| `analyze exit 2` | The app log is not in `$OUT/applog` (was `$OUT` set in this window?). `ls "$OUT/applog"` shows `curia.log`. If it is there, tell the maintainer the tool's message. |
| `grep_key exit 3` (not complete) | Some file or folder could not be read in full; the lines `could not read` and `partly scanned` name them. Tell the maintainer those names. |
| What you type appears in Terminal or reaches the game during A7 to A9 | The keys were elsewhere after a click outside the overlay. Click once into the overlay's input box, then type. |
