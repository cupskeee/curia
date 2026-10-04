# Milestone 2 session checklist (owner, macOS 26.5.1, one sitting)

Status: scope, stop rules and the changes below were confirmed by the owner on 2026-10-05. The probe (`app/probe/`) and the stub mod (`mod/curia-m2-probe/`) are written; section 1 has the build command and section 3 the launch line for every stage. Both were exercised without CK3 only (the probe's stages ran, logged and exited on their own, and the code was reviewed; the mod has never been loaded by the game). Nothing in this sheet is a result: every "does it work" below is answered by running it (hard rule 4). Background and sources: `research-notes.md` §2.5, §5.2, C26, C33.

## Scope (owner decision 2026-10-05)
Milestone 2 acceptance criteria **1–5 and 8** on the Mac, plus the **T7 stub-widget probe** in the same session. Windows and Linux (criteria 6–7) are CI plus a separate tester checklist. Primary target: CK3 **Fullscreen** (a native macOS Space). First fallback: **Windowed**. Third rung: a normal companion window (FR-OVL-8); the probe does not test that rung (it is derived if E3 and E10 fail, and assumed to work).

There is no clock in this sheet (the session length is the owner's call). It is ordered so that you can stop after any experiment and the table so far is still useful.

**Stop rules (confirmed by the owner 2026-10-05)**
- E3 decides the Fullscreen overlay experiments. If the overlay is invisible over the native Space at every level and flag combination in E3, record that, skip E4–E7 for Fullscreen, and continue with E10 (Windowed).
- E8 (hotkey), E9 (game detection and rectangle) and E11 (tray) do not depend on the overlay being visible: run them in any case.
- If E10 also fails, record it: the result is rung 3 (a normal companion window); E8, E9 and E11 still run.
- Stop immediately if CK3 crashes, loses its Space repeatedly, or a permission prompt appears that you did not expect; write down the exact prompt text and which app name it shows.

## 1. The probe and the stub mod
**Overlay probe** (`app/probe/`): a macOS app bundle (owner decision 2026-10-05: built as a `.app` and launched with `open`, not started from a terminal, so that a permission prompt, if one appears, concerns the probe itself and not the launching process). It uses the same stack as the real overlay (SDL3 window wrapped in our own `NSPanel`, `SDL_Renderer`, Dear ImGui). Each experiment is a named stage.

**Timeline of a stage:** you launch it from a terminal; the probe waits a **start delay** (15 s by default, `--start-delay S` to change, `0` to skip) with its window hidden, during which you click into CK3 and leave the mouse pointer on CK3's display; then a **"go" sound** (the system sound "Glass") marks the start, the window appears on the display under the pointer (`--display N` to force one; 0 is the primary display) and the stage runs. **E3 runs by itself:** it applies each of the 20 level and flag combinations for 5 s (`--step-seconds`), in the same way each time (hide, set, show), shows the combination number in the window, plays a short system sound ("Tink") at each switch, logs what was applied and, 1.5 s after each switch, the settled state; a 21st step replays combination 1 to show whether the order matters. So you only note combination numbers (or count sounds) instead of switching settings by hand. Every stage logs the frontmost application, whether the probe is the active app, whether the panel is the key window and what the permission pre-checks return.

Build (from your curia checkout):
```sh
export VCPKG_ROOT=~/vcpkg
cmake --preset macos-arm64 && cmake --build --preset macos-arm64 --target curia_m2_probe
APP="$PWD/build/macos-arm64/app/curia_m2_probe.app"; ls "$APP/Contents/MacOS/"
```
The bundle is ad-hoc signed by the build and has its own identifier (`io.github.cupskeee.curia.m2probe`), so any privacy-settings entry it causes is separate from the real app's. Keep `APP` set in the terminal you launch from (launching with `open` does not tie the probe to that terminal).

**Log files:** every run writes `~/curia_m2_results/probe-<stage>-<date>-<time>.log` (one line per event, flushed immediately). If the arguments are wrong the reason is written to `probe-error-*.log` in the same folder.

**Stopping a probe:** it ends by itself after the stage's planned time counted from "go" (E1/E2 20 s, E3 105 s, E4 60 s; `interactive` and `tray` run until you quit; `detect` 60 s from launch) and always 900 s after launch (`--max-seconds`). Quit earlier with the hot key **Ctrl+Cmd+J** (registered in every stage; its registration result is the `hotkeys_registered` line in the log), the **Quit** button or tray entry, or from a terminal: `pkill -f curia_m2_probe`. **One probe at a time:** a second launch while one is running refuses to start (it plays the system sound "Basso" and writes `another_instance_running` to its log), because the hot keys can only be registered once; quit the previous one first.

**Stub mod** (`mod/curia-m2-probe/`, for T7): a HUD button whose click writes one `CURIAPROBE1|T7|click|` line to `debug.log`, plus the helper `tools/m2/check_t7.sh`. Install and cleanup commands are in section 4.

## 2. Before you start (record, do not change)
- macOS 26.5.1 arm64, CK3 1.20.0.3. Number of displays.
- System Settings → Desktop & Dock → Mission Control: **"Displays have separate Spaces"** (on/off) and **Stage Manager** (on/off). **Do not toggle them** (the macOS 26 notes list a WindowServer crash at login when separate Spaces is off).
- System Settings → Privacy & Security: note which apps are listed under **Accessibility**, **Input Monitoring** and **Screen & System Audio Recording** now (for the end-of-session comparison).
- CK3: normal launch (never debug mode), no console, a **throwaway save**. After loading, check the achievements indicator (pause menu icon next to Game Rules) and record it.

## 3. Experiments
Run each for **Fullscreen**; E10 repeats the useful ones in **Windowed**. Record what you see, not what you expect. The probe's log file records the focus facts; you note the visible ones. Keep Terminal and CK3 on the same display for the session (the probe places its window on the display under the pointer at "go"; the `go` log line says which one it chose).

| # | Do | Record | Criterion |
|---|----|--------|-----------|
| E1 | Control: plain window (always-on-top, transparent, Regular policy) while CK3 is Fullscreen; run it with `-g` (background launch) and once without | invisible / visible; did CK3 leave its Space; did the desktop Space appear; whether launching without `-g` changes it | 1 (baseline) |
| E2 | Same, Accessory policy only | same three | 1 |
| E3 | Panel recipe, **opaque** window, **automatic**: the probe cycles through the levels (floating, status, pop-up menu, screen saver) and flag sets (including `canJoinAllApplications` and without `stationary`), a sound at each switch, combination 1 replayed at the end | the combination numbers shown in the window when you saw it over CK3; for any stretch where you saw nothing, count the sounds after the first, higher one (one per step); the log gives the applied combination and, 1.5 s later, the settled state (`why=settled`: occlusion, on-active-space) | 1 |
| E4 | Same panel, **transparent** with alpha-0 clear | transparent areas show the game; dark fringe around edges; shadow; the `cpu` lines in the log every 10 s (use the **differences** between lines; the first includes start-up; the probe's own timer wakes once per second in this stage) | 4 |
| E5 | Click the overlay; then try whole-window click-through (button or Ctrl+Cmd+L; per-pixel pass-through is not tested) | after the click, is CK3 still the frontmost app (probe log) and did the game pause or lose the mouse; what happens to clicks in click-through mode and what brings interactivity back | 1, 3 |
| E6 | Focus the overlay's text field and type letters, spaces and the digits 1–5 | do the characters appear; does CK3 react (pause toggle, speed change); does the cursor stay captured by CK3 | 1, 3 |
| E7 | Dismiss with Esc / Ctrl+Cmd+K | do CK3 keys work again **without a click** (press Space and watch the pause state); if not, try the escalation: the button "Re-activate previous app" or Ctrl+Cmd+H (asks the system to make the app that was frontmost at "go" frontmost again; the log records the request, whether it was accepted and the state 0.5 s later); flicker | 1, 3 |
| E8 | Global hotkey toggles the overlay while CK3 is frontmost: a Cmd or Ctrl based combination only (the default binding requires one of them; Option-only is not tested, owner decision 2026-10-05) | works / does nothing; whether CK3 also reacts to the key (not determinable unless it reacts visibly); **any prompt** (exact text, app name) | 2 |
| E9 | Game detection and window rectangle: `detect` logs the frontmost app (name, executable, bundle id; the installed `Info.plist` shows executable `ck3` and an empty bundle id), CK3's window bounds, layer and display, and the bounds of every display | populated or empty; any prompt; whether the rect equals the display bounds (Fullscreen) or a smaller window (Windowed); the `layer` of CK3's windows; `displays_captured` comes from a CoreGraphics call deprecated since macOS 10.9, so `0` is not proof that a display is not captured | 8 |
| E10 | **Windowed** CK3: repeat E3–E8; also compare SDL's plain always-on-top window (E1/E2 style, level 3) with the panel | per experiment, as above | 1–4 |
| E11 | Tray: menu-bar item with Show / Hide / Quit; created **before** CK3 is frontmost once and **while** CK3 is frontmost once (launch lines below) | works; Dock icon shown or not; did creating it take focus from CK3 (the `tray_create_begin` and `tray_create_end` lines show the focus state around the call) | 5 |

**Launch lines** (run in a terminal; the probe does not need the terminal afterwards; `-g` keeps it from being brought to the front, `-n` starts a fresh instance; after each launch click into CK3 and wait for the "go" sound):

| Stage | Command | Notes |
|-------|---------|-------|
| E1 | `open -g -n "$APP" --args --stage e1` and `open -n "$APP" --args --stage e1` | control: SDL's own behaviour (Regular policy, activates on show); the first is a background launch, the second without `-g`; it may pull CK3 out of its Space, that is the result; 20 s after "go" |
| E2 | `open -g -n "$APP" --args --stage e2` | control: Accessory policy after start, SDL's default activation |
| E3 | `open -g -n "$APP" --args --stage e3` | 20 combinations plus the replay, 5 s each (`--step-seconds 8` for slower); window shows `COMBO n / 20`; sounds at each switch (`--no-sound` to mute, which also removes the "go" cue) |
| E3 hold | `open -g -n "$APP" --args --stage e3 --combo N --seconds 60` | keeps combination `N` (for example to try Space switching) |
| E4 | `open -g -n "$APP" --args --stage e4 --combo N` | `N` = a combination that was visible in E3 (default 1) |
| E5–E8 | `open -g -n "$APP" --args --stage interactive --combo N` | hot keys Ctrl+Cmd+K show/hide, L click-through (10 s), H re-activate the previous app, J quit; Esc dismisses; the window counts clicks and keys and shows the hot-key registration result |
| E9 | `open -g -n "$APP" --args --stage detect --seconds 90` | no window and no start delay; switch between CK3 and other apps while it runs |
| E11 before | `open -g -n "$APP" --args --stage tray --combo N --start-delay 0` | tray item created at once, while Terminal is frontmost; then click into CK3 |
| E11 while | `open -g -n "$APP" --args --stage tray --combo N` | tray item created at "go", with CK3 frontmost |

About E11: SDL 3.4.18's tray code (`src/tray/cocoa/SDL_tray.m`) calls an application-activation function unconditionally when the item is created, so some focus effect is to be expected and the log records it; in a Fullscreen Space the menu bar is hidden until the pointer reaches the top edge of the screen; the menu-bar icon is an orange square.

Caveats, so they are not mistaken for failures: in the self-test runs without CK3 the panel logged itself as active and key right after the first show while another app stayed frontmost (the log is the record, whatever it says in CK3); the panel is hidden and shown again for every combination so all of them are applied the same way (whether that matters is not known; the final replay of combination 1 is the check); the sounds are macOS system sounds.

**End of experiments:** open Privacy & Security again and note any new entry under Accessibility, Input Monitoring, Screen & System Audio Recording. The pre-check results are the `permissions_start` and `permissions_end` lines in every probe log (`detect` repeats them every 2 s); copy them next to your notes.

## 4. T7 probe (stub widget, same session)
**Install** (CK3 fully quit; from your curia checkout; the stub is not installed now):
```sh
REPO="$PWD"; MODDIR="$HOME/Documents/Paradox Interactive/Crusader Kings III/mod"
rm -rf "$MODDIR/curia_m2_probe" && mkdir -p "$MODDIR/curia_m2_probe" && cp -R "$REPO/mod/curia-m2-probe/." "$MODDIR/curia_m2_probe/"
printf 'version="0.0.1"\ntags={\n\t"Utilities"\n}\nname="Curia M2 Probe"\nsupported_version="1.20.*"\npath="%s"\n' "$MODDIR/curia_m2_probe" > "$MODDIR/curia_m2_probe.mod"
ls -l "$MODDIR/curia_m2_probe.mod" "$MODDIR/curia_m2_probe/descriptor.mod" "$MODDIR/curia_m2_probe/gui" "$MODDIR/curia_m2_probe/common/"*
```
The listing must show the `.mod` file, the descriptor, `curia_m2_probe_widget.gui` with the `scripted_widgets` folder, and the `on_action` and `scripted_guis` folders. In the Paradox Launcher check that "Curia M2 Probe" is listed and **enabled** in the playset (add it if missing; do not launch yet), then **Play** (normal launch, never debug mode).

**Suggested order for the whole session** (fewest CK3 restarts): build the probe (section 1); install the stub (above); start CK3 with the stub enabled; Graphics → **Fullscreen**; start a **new** throwaway game (the stub's load line is written only at the start of a new game); run the Fullscreen experiments and the T7 clicks below; then Graphics → **Windowed** for E10 (and the T7 button check in Windowed); quit CK3; clean up.

1. Is the button "Curia T7 probe" visible in the HUD (expected near the bottom left, to the right of the bottom-left HUD bar; the position is a guess from reading the game's files) in **Fullscreen** and in **Windowed**? Where?
2. Is it clickable (cursor over it highlights; click does something)?
3. Click it, then run `tools/m2/check_t7.sh <label>`: does a `CURIAPROBE1|T7|click|` line appear, and does the character name in it match your ruler (root scope = the player)? The date field may print empty; the separate `DATE:` line after it is the fallback. How soon `debug.log` is flushed is not known (it is a milestone M3 test), so if there is no click line right after clicking, wait 30 seconds and run the helper again, then quit CK3 and run it once more before concluding that the click did nothing, and note when the line first appeared.
4. **Paused and unpaused:** click a few times in each state, noting the order by hand; the helper output (line order and timestamps) is matched against your notes. Use labels such as `paused` and `unpaused`.
5. Achievements indicator after loading with the stub active: compare with section 2.
6. Anything in CK3's `error.log` mentioning the stub: section 4 of the helper output lists it.

**Cleanup** (after the results are in; CK3 quit):
```sh
MODDIR="$HOME/Documents/Paradox Interactive/Crusader Kings III/mod"
rm -rf "$MODDIR/curia_m2_probe" "$MODDIR/curia_m2_probe.mod"; ls -l "$MODDIR"
```
Then remove "Curia M2 Probe" from the launcher playset if it still shows.

## 5. Recording sheet
Criterion 1 (one row per display mode):

| Mode | Visible above CK3 (level, flags that worked) | Clickable | Takes typed input | Focus on close | Notes |
|------|----------------------------------------------|-----------|-------------------|----------------|-------|
| Fullscreen | | | | | |
| Windowed | | | | | |

Which rung of the ladder works: ______ (1 overlay over Fullscreen / 2 overlay with Windowed / 3 normal companion window)

Criteria 2 (hotkey prompt: yes/no, text), 3 (CK3 stays in front: yes/no, exceptions), 4 (fringe: yes/no; CPU TIME delta), 5 (tray and Dock behaviour), 8 (rect: ok / prompt / empty), T7 (items 1–6).

## 6. Afterwards
Send back the filled tables and the contents of `~/curia_m2_results/` (probe logs `probe-*.log` and the helper's `t7-*.txt` files). The answers go into `research-notes.md` §5.2 (marked answered), `milestones.md` M2 (status and the ladder result) and, if the recipe fails, a re-plan before any further macOS overlay code.
