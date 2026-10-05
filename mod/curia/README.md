# Curia mod (mod id `curia`), M3 spike build 0.1.0

The CK3 side of Curia: two HUD buttons that write a read-only snapshot of the player's realm to CK3's `debug.log`
as `CURIA1|...` lines, for the Curia app to read. **This is the Milestone 3 spike build. It is not released and not
published**; its job is to let the maintainer measure the log (flush latency, line format, bytes per export) with the
real export shape. Frame, field order and the set of exported sections can still change; nothing is frozen until
`docs/wire-format.md` exists.

It overrides no vanilla file (every file name carries the `curia_` prefix, no `replace_path`) and needs no console and no
`-debug_mode`. It is written to leave `error.log` untouched in normal use; only static checks (the structural lint and
ck3-tiger) have been run, the in-game check is an owner test. Curia keeps six counters as global variables in the save (`curia_snap_id`,
`curia_export_lines`, `curia_rank`, `curia_war_index`, `curia_claim_index`, `curia_probe_mode`); nothing else in the
game state is touched.

## Buttons (bottom left of the HUD, visible in the default game view)
- **Advisor** runs the export: BEGIN, FIN, HEIR, VASSAL, WAR, CLAIM, END.
- **Curia probes** (spike only) runs the same export plus the PROBE section before END.

Both run a scripted GUI (`curia_run_advisor`, `curia_run_probes`) with the player as root scope. The first button sits at
the position the M2 stub proved clear of the HUD; the second is to its right and has not yet been seen on screen.

## Files
```
descriptor.mod                                     name, version 0.1.0, tags Utilities, supported_version 1.20.* (no BOM)
.metadata/metadata.json                            launcher metadata, same name/version/supported version, id "curia"
gui/curia_widget.gui                               the widget with the two buttons
gui/scripted_widgets/curia_widgets.txt             registers the widget (no vanilla GUI file overridden)
common/scripted_guis/curia_scripted_guis.txt       curia_run_advisor, curia_run_probes (scope character)
common/scripted_effects/curia_export.txt           the export, one effect per section
common/scripted_effects/curia_probes.txt           the probe section, one effect per group
common/script_values/curia_values.txt              caps, frame counters, opinion and flag values, probe-only values
common/on_action/curia_on_actions.txt              the game-start line CURIA1|0|LOAD|mod=curia|version=0.1.0
localization/english/curia_l_english.yml           button texts and every logged line (UTF-8 BOM, l_english:)
```
Script and localization files carry a UTF-8 BOM and tabs, like the game's own; `python3 tools/ci/lint_mod.py` checks that.

## Install and remove (maintainer, macOS; quit CK3 completely first)
```sh
tools/m3/install_mod.sh                        # installs into ~/Documents/Paradox Interactive/Crusader Kings III/mod
tools/m3/install_mod.sh --variant descriptor   # T11 (deferred to M7): descriptor.mod only; --variant metadata: .metadata/metadata.json only
tools/m3/install_mod.sh --supported-version 1.19.*   # T11 (deferred to M7): rewrites supported_version in the installed copy only
tools/m3/install_mod.sh --uninstall            # removes <mod folder>/curia and curia.mod
```
Options `--mod-dir <dir>`, `--dry-run` and `--force` exist; `--help` lists them. The script refuses to run while CK3 runs and
never modifies this repository copy. Normal launch only (never debug mode), new game: the `LOAD` line is written at the
start of a new game; whether loading an existing save prints it is not known (the game has no on-load hook that was found).

## Line frame
Every line starts with `CURIA1|<snap>|`; `<snap>` is a counter that goes up by one per export. Fields are separated by `|`.
Names come from the `NoTooltip` forms, so they include titles ("King Harold of England"). Numbers are plain integers.
`none` lines mark a section that ran and found nothing.

| Line | Fields after `<snap>\|` |
|---|---|
| `BEGIN` | `BEGIN\|<date>\|<ruler name>\|<ruler primary title>\|<probe mode 0/1>` |
| `FIN` | `FIN\|<gold>` |
| `HEIR` | `HEIR\|<name>\|<opinion of the ruler>\|<has liege 0/1>\|<landed 0/1>`, or `HEIR\|none` |
| `VASSAL` | `VASSAL\|<rank>\|<name>\|<primary title>\|<gold>\|<opinion of the ruler>` (top N by military strength, N = `curia_max_vassals`, default 10), or `VASSAL\|0\|none` |
| `WAR` | three lines per war: `WAR\|<n>\|NAME\|<war name>`, `WAR\|<n>\|ATTACKER\|<name>`, `WAR\|<n>\|DEFENDER\|<name>`; or `WAR\|0\|none` |
| `CLAIM` | `CLAIM\|<n>\|<title>` (highest tier first, at most `curia_max_claims`, default 10), or `CLAIM\|0\|none` |
| `PROBE` | `PROBE\|<name>\|<value>`, probe button only, one candidate per line |
| `END` | `END\|<line count>`: the number of lines in the frame, BEGIN and END included |

A war is three lines because the war name is only reachable from the war scope and the side names only from their own
scopes (the proven forms cannot be combined in one line). `ATTACKER` and `DEFENDER` are the war's primary sides and the ruler may only be a participant (for example in an ally's war), so the app compares the names
with the BEGIN name instead of assuming a side; whether the war scope offers a link to the ruler's own side is a game test. A frame is complete when `END` arrives and its count equals the lines seen for that `<snap>`.
The engine puts about 110 bytes in front of every line (script path, effect name), so the parser finds `CURIA1|` anywhere in
the line.

## Forms: proven in M0/M0b versus used here but not proven
Proven (`docs/spikes/m0-results.md`, T7 in `docs/spikes/m2-results.md`): `[THIS.GetCharacter.GetNameNoTooltip]`,
`[THIS.GetCharacter.GetGold|0]`, `[THIS.GetCharacter.GetPrimaryTitle.GetNameNoTierNoTooltip]`, `[THIS.War.GetName]`,
`[THIS.Title.GetNameNoTierNoTooltip]`, `[GetCurrentDate.GetString]`, `[THIS.ScriptValue('x')|0]`, `[EmptyScope.ScriptValue('x')|0]`
(with a constant), the script value `"opinion(liege)"`, the iterators `player_heir`, `ordered_vassal` (`max`, `order_by =
current_military_strength`), `every_character_war` with `primary_attacker` / `primary_defender`, `random_claim`.

Used in the export but **not** proven at runtime (written from the game's own files; each is a possible empty field):
- `[EmptyScope.ScriptValue('x')|0]` where the script value reads a global variable (`global_var:`): the snapshot id, the line
  count and the rank/war/claim numbers all depend on it. The probe `snap_id_this` reads the same value through `THIS`.
- `set_global_variable` / `change_global_variable` used as counters.
- `max = curia_max_vassals` (a named script value; M0b used a literal number).
- `ordered_claim` with `max` and `order_by = tier` (vanilla uses it that way; M0b ran `random_claim`, which has the same title scope).
- The heir's opinion through a guarded script value (`if exists = liege` around `add = "opinion(liege)"`) and the `landed` flag
  (script value around the `is_landed` trigger).

## Probe section (candidates, none proven)
Each line tests one form, so a failure shows up as an empty or 0 value on that line only. Groups: ruler, heir, strongest
vassal, highest-tier claim, one war, factions that target the ruler.

| Probe name | Form under test |
|---|---|
| `snap_id_this` | the snapshot-id script value read through `THIS` instead of `EmptyScope` |
| `ruler_id`, `heir_id`, `vassal_id` | `[THIS.GetCharacter.GetID]` |
| `ruler_first_name`, `heir_first_name`, `vassal_first_name` | `[THIS.GetCharacter.GetFirstNameNoTooltip]` (names without titles) |
| `ruler_income_monthly`, `ruler_income_yearly` | script values over `monthly_character_income`, `yearly_character_income` |
| `ruler_strength`, `vassal_strength` | script value over `current_military_strength` |
| `ruler_realm_size` | script value over `realm_size` |
| `ruler_landless_ruler`, `heir_landless_ruler` | script value over the `is_landless_ruler` trigger |
| `vassal_landed` | the `is_landed` flag value in the vassal scope (the same value the HEIR line uses) |
| `vassal_count_trigger`, `vassal_count_iterator`, `vassal_count_below` | `vassal_count`; `every_vassal` counting; `every_vassal_or_below` counting |
| `ruler_claim_count`, `heir_claim_count`, `vassal_claim_count` | script value with `every_claim` counting |
| `claim_tier` | script value over `tier` in the title scope |
| `war_score`, `war_start_date`, `war_enemy` | `[THIS.War.GetWarScore|0]`, `[THIS.War.GetStartDate.GetString]`, `[THIS.War.GetPrimaryPlayerEnemy.GetNameNoTooltip]` |
| `faction_name` | `[THIS.Faction.GetNameNoTooltip]` |
| `faction_power_df`, `faction_discontent_df` | `[THIS.Faction.GetPower|0]`, `[THIS.Faction.GetDiscontent|2]` |
| `faction_power_sv`, `faction_discontent_sv` | script values over `faction_power`, `faction_discontent` in the faction scope |
| `faction_member_count` | script value with `every_faction_member` counting |
| `faction_leader`, `faction_target` | the `faction_leader` / `faction_target` scope links (guarded with `exists`; `PROBE\|faction_leader\|none` when absent) |

Non-ASCII name handling needs no code: it is observed from real names in the capture.

To turn probes off: remove `curia_export_probes = yes` from `curia_export_run`, or one group line from `curia_export_probes`,
or one `change_global_variable` + `debug_log` pair (all in `common/scripted_effects/`). No probe is known to raise a script
error: the scope links that could (`faction_leader`, `faction_target`, `player_heir`, vassals, wars, claims) are guarded, and the
rest are data functions and script values that print an empty value or 0 when they do not resolve. The ones that carry the
most risk, because faction scope was never run: the `faction_*` probes. A data function that does not exist would be
reported when the localization loads (`error.log`); ck3-tiger found none.

## Spike limits
- The two HUD buttons span x 570 to 866 (bottom|left anchor) and were checked clear of the HUD only in the owner's 2056-point-wide Fullscreen view. The vanilla bottom-right block is 630 wide, so on a UI narrower than about 1,400 units the probe button can overlap it; the shipped mod anchors relative to the bottom-left block instead.
