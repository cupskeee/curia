# curia-m2-probe: throwaway Milestone 2 probe (T7), install only for the M2 session

A minimal stub mod with ONE small test button on the CK3 HUD. It is **not** the Curia mod, it is never shipped, and it
must not stay installed after the M2 session. It overrides no vanilla GUI or script file (new `curia_`-prefixed file names
only, no `replace_path`) and writes nothing to `error.log` in normal use.

What it does:
- Clicking the button runs a scripted GUI effect as the player and writes ONE `debug.log` line starting with
  `CURIAPROBE1|T7|click|` (marker is deliberately not the real `CURIA1|`), followed by the date, the character's name,
  primary title and gold. The engine's `debug_log_date` adds a separate unmarked `DATE:` line right after it.
- At the start of a **new** game it writes one `CURIAPROBE1|T7|load|` line so the helper can tell the mod loaded.
  The game has no on-load hook, so loading an existing save does not print it.

Files:
- `descriptor.mod`
- `gui/curia_m2_probe_widget.gui` (the button window; `layer = hud_layer`, anchored bottom left, placed to the right of the widest bottom-left HUD bar found in the game's files; the position is a guess; the button hides outside the default game view)
- `gui/scripted_widgets/curia_m2_probe_widgets.txt` (registers the widget so the game spawns it; no vanilla file override)
- `common/scripted_guis/curia_m2_probe_scripted_guis.txt` (the click effect)
- `common/on_action/curia_m2_probe_on_actions.txt` (the load line)
- `localization/english/curia_m2_probe_l_english.yml` (button text, click line text)

## Install (owner, macOS; quit CK3 completely first)
Run from your curia checkout. Normal launch only: **never** use debug mode, and use a **throwaway** save (new game, Ironman off).
```sh
REPO="$PWD"; MODDIR="$HOME/Documents/Paradox Interactive/Crusader Kings III/mod"
rm -rf "$MODDIR/curia_m2_probe" && mkdir -p "$MODDIR/curia_m2_probe" && cp -R "$REPO/mod/curia-m2-probe/." "$MODDIR/curia_m2_probe/"
printf 'version="0.0.1"\ntags={\n\t"Utilities"\n}\nname="Curia M2 Probe"\nsupported_version="1.20.*"\npath="%s"\n' "$MODDIR/curia_m2_probe" > "$MODDIR/curia_m2_probe.mod"
ls -l "$MODDIR/curia_m2_probe.mod" "$MODDIR/curia_m2_probe/descriptor.mod" "$MODDIR/curia_m2_probe/gui" "$MODDIR/curia_m2_probe/common/"*
```
The listing must show the `.mod` file, the descriptor, `curia_m2_probe_widget.gui` plus the `scripted_widgets` folder, and the
`on_action` and `scripted_guis` folders. Then open the Paradox Launcher, check the playset: "Curia M2 Probe" must be listed and
**enabled** (if missing or greyed out, add it to the playset; do not launch yet). Launch with the normal **Play** button.

## Run
1. Start a **new** game (throwaway). The button "Curia T7 probe" should appear on the HUD at the bottom left, to the right of the bottom-left HUD bar (the position is a guess).
2. Note by hand whether the game is paused or unpaused at each click, and click the button a few times in each state.
3. Collect the log lines: `tools/m2/check_t7.sh <label>`. Output is saved under `~/curia_m2_results/`. How soon `debug.log` is flushed is not known (milestone M3 test): if no click line shows right away, wait 30 seconds and run it again, then quit CK3 and run it once more, and note when the line first appeared.

## Cleanup (after the results are in; quit CK3 first)
```sh
MODDIR="$HOME/Documents/Paradox Interactive/Crusader Kings III/mod"
rm -rf "$MODDIR/curia_m2_probe" "$MODDIR/curia_m2_probe.mod"
ls -l "$MODDIR"
```
Then remove "Curia M2 Probe" from the launcher playset if it still shows. The mod is **not installed on the owner's disk now**: nothing has been
copied into the game's mod folder; the only copy is in this repository.
