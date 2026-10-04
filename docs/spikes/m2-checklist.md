# Milestone 2 session checklist (owner, macOS 26.5.1, one sitting)

Status: scope, stop rules and the changes below were confirmed by the owner on 2026-10-05. **The probe and the stub mod listed in section 1 are not written yet**, so no launch commands appear here. Nothing in this sheet is a result: every "does it work" below is answered by running it (hard rule 4). Background and sources: `research-notes.md` §2.5, §5.2, C26, C33.

## Scope (owner decision 2026-10-05)
Milestone 2 acceptance criteria **1–5 and 8** on the Mac, plus the **T7 stub-widget probe** in the same session. Windows and Linux (criteria 6–7) are CI plus a separate tester checklist. Primary target: CK3 **Fullscreen** (a native macOS Space). First fallback: **Windowed**. Third rung: a normal companion window (FR-OVL-8).

There is no clock in this sheet (the session length is the owner's call). It is ordered so that you can stop after any experiment and the table so far is still useful.

**Stop rules (confirmed by the owner 2026-10-05)**
- E3 decides the Fullscreen overlay experiments. If the overlay is invisible over the native Space at every level and flag combination in E3, record that, skip E4–E7 for Fullscreen, and continue with E10 (Windowed).
- E8 (hotkey), E9 (game detection and rectangle) and E11 (tray) do not depend on the overlay being visible: run them in any case.
- If E10 also fails, record it: the result is rung 3 (a normal companion window); E8, E9 and E11 still run.
- Stop immediately if CK3 crashes, loses its Space repeatedly, or a permission prompt appears that you did not expect; write down the exact prompt text and which app name it shows.

## 1. Built before the session (not done yet)
1. **Overlay probe**: a macOS app bundle (owner decision 2026-10-05: built as a `.app` and launched with `open`, not started from a terminal, so that a permission prompt, if one appears, concerns the probe itself and not the launching process). It uses the same stack as the real overlay (SDL3 window wrapped in our own `NSPanel`, `SDL_Renderer`, Dear ImGui). Each experiment is a named stage; **E3 runs by itself**: the probe cycles through the level and flag combinations on a timer (a few seconds each), shows the combination number in the window, plays a short sound at each switch and logs which combination was active at what time, so you only note combination numbers or clock times instead of switching settings by hand. Every stage also logs (to a file) the frontmost application, whether the probe is the active app, whether the panel is the key window and what the permission pre-checks return.
2. **Stub mod** for T7 (throwaway, under `mod/`): a HUD widget button whose click writes a marker line to `debug.log`, plus a helper to read it. Same install pattern as the M0 stubs.
3. Exact commands for launching the probe and installing the stub go into this sheet when both exist.

## 2. Before you start (record, do not change)
- macOS 26.5.1 arm64, CK3 1.20.0.3. Number of displays.
- System Settings → Desktop & Dock → Mission Control: **"Displays have separate Spaces"** (on/off) and **Stage Manager** (on/off). **Do not toggle them** (the macOS 26 notes list a WindowServer crash at login when separate Spaces is off).
- System Settings → Privacy & Security: note which apps are listed under **Accessibility**, **Input Monitoring** and **Screen & System Audio Recording** now (for the end-of-session comparison).
- CK3: normal launch (never debug mode), no console, a **throwaway save**. After loading, check the achievements indicator (pause menu icon next to Game Rules) and record it.

## 3. Experiments
Run each for **Fullscreen**; E10 repeats the useful ones in **Windowed**. Record what you see, not what you expect. The probe's log file records the focus facts; you note the visible ones.

| # | Do | Record | Criterion |
|---|----|--------|-----------|
| E1 | Control: plain window (always-on-top, transparent, Regular policy) while CK3 is Fullscreen | invisible / visible; did CK3 leave its Space; did the desktop Space appear | 1 (baseline) |
| E2 | Same, Accessory policy only | same three | 1 |
| E3 | Panel recipe, **opaque** window, **automatic**: the probe cycles through the levels (floating, status, pop-up menu, screen saver) and flag sets (including `canJoinAllApplications` and without `stationary`) on a timer, showing the combination number in the window, with a sound at each switch | the combination numbers (shown in the window) or clock times at which you saw the window over CK3; the probe's log gives the timeline | 1 |
| E4 | Same panel, **transparent** with alpha-0 clear | transparent areas show the game; dark fringe around edges; shadow; the **probe process's** CPU TIME (Activity Monitor, or `ps -o time -p <pid>`) before and after a fixed interval you note, with the overlay open and static | 4 |
| E5 | Click the overlay; then try click-through mode | after the click, is CK3 still the frontmost app (probe log) and did the game pause or lose the mouse; do clicks pass through transparent areas in click-through mode and what brings interactivity back | 1, 3 |
| E6 | Focus the overlay's text field and type letters, spaces and the digits 1–5 | do the characters appear; does CK3 react (pause toggle, speed change); does the cursor stay captured by CK3 | 1, 3 |
| E7 | Dismiss with Esc / the hotkey | do CK3 keys work again **without a click** (press Space and watch the pause state); if not, which step restores it (probe offers escalation options); flicker | 1, 3 |
| E8 | Global hotkey toggles the overlay while CK3 is frontmost: a Cmd or Ctrl based combination only (the default binding requires one of them; Option-only is not tested, owner decision 2026-10-05) | works / does nothing; does CK3 also receive the key; **any prompt** (exact text, app name) | 2 |
| E9 | Game detection and window rectangle: read the frontmost-app name and bundle id (the installed `Info.plist` shows executable `ck3` and an empty bundle id; record what the running process reports) and the window bounds for CK3's process | populated or empty; any prompt; whether the rect equals the display (Fullscreen) or the window (Windowed) | 8 |
| E10 | **Windowed** CK3: repeat E3–E8; also compare SDL's plain always-on-top window (level 3) with the panel | per experiment, as above | 1–4 |
| E11 | Tray: menu-bar item with Show / Hide / Quit; create it **before** CK3 is frontmost once and **while** CK3 is frontmost once | works; Dock icon shown or not; did creating it take focus from CK3 | 5 |

**End of experiments:** open Privacy & Security again and note any new entry under Accessibility, Input Monitoring, Screen & System Audio Recording. Run the probe's permission pre-check print and record each result.

## 4. T7 probe (stub widget, same session)
Install the stub mod (steps added when it exists), normal launch, throwaway save.
1. Is the button visible in the HUD in **Fullscreen** and in **Windowed**? Where?
2. Is it clickable (cursor over it highlights; click does something)?
3. Does a click write the marker line (run the helper)? Is the printed character the player (root scope)?
4. **Paused and unpaused:** same click in both.
5. Achievements indicator after loading with the stub active: compare with section 2.
6. Anything in CK3's `error.log` mentioning the stub (the helper can list it).
Afterwards remove the stub mod and its `.mod` file (cleanup steps added with the stub).

## 5. Recording sheet
Criterion 1 (one row per display mode):

| Mode | Visible above CK3 (level, flags that worked) | Clickable | Takes typed input | Focus on close | Notes |
|------|----------------------------------------------|-----------|-------------------|----------------|-------|
| Fullscreen | | | | | |
| Windowed | | | | | |

Which rung of the ladder works: ______ (1 overlay over Fullscreen / 2 overlay with Windowed / 3 normal companion window)

Criteria 2 (hotkey prompt: yes/no, text), 3 (CK3 stays in front: yes/no, exceptions), 4 (fringe: yes/no; CPU TIME delta), 5 (tray and Dock behaviour), 8 (rect: ok / prompt / empty), T7 (items 1–6).

## 6. Afterwards
Send back the filled tables and the probe log file(s). The answers go into `research-notes.md` §5.2 (marked answered), `milestones.md` M2 (status and the ladder result) and, if the recipe fails, a re-plan before any further macOS overlay code.
