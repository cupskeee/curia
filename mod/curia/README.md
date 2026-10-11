# Curia mod (mod id `curia`), version 0.2.1

The CK3 side of Curia: two HUD buttons that write a read-only snapshot of the player's realm to CK3's `debug.log`
as `CURIA1|...` records, for the Curia app to read. **Not released and not published.** Version 0.2.1 writes the format
approved at the wire-format checkpoint of 2026-10-11 (`docs/wire-format.md`, the single source of truth for the records);
the format was **frozen** on 2026-10-11 after the maintainer's pre-freeze in-game check (`docs/wire-format.md` section 9, results in
`docs/spikes/m3-prefreeze-results.md`); section 8 of that file says which forms ran and which did not. The 0.1.0 spike layout is gone and is not
readable by the new format; the mod and the app change together.

It overrides no vanilla file (every file name carries the `curia_` prefix, no `replace_path`) and needs no console and no
`-debug_mode`. It is written to leave `error.log` untouched in normal use (the checks that ran in M3 are about 0.1.0; 0.2.0 ran in the pre-freeze
check of 2026-10-11 with no `error.log` entry; 0.2.1 only changes the income figure and adds probes, and has had the structural lint, its own format lint and ck3-tiger, no game run). Curia keeps these counters as global variables
in the save: `curia_snap_id`, `curia_export_lines`, `curia_rank`, `curia_war_index`, `curia_claim_index`,
`curia_faction_index`, `curia_vassals_listed`, `curia_wars_total`, `curia_claims_listed`, `curia_factions_listed` and
`curia_probe_mode`; in probe mode it also sets four claim-flag variables (`curia_claim_f_es`, `curia_claim_f_ew`,
`curia_claim_f_is`, `curia_claim_f_iw`). Nothing else in the game state is touched.

## Buttons (bottom left of the HUD, visible in the default game view)
- **Advisor** runs the export: BEGIN, FIN, HEIR, VASSALS, WARS, CLAIMS, FACTIONS, END.
- **Curia probes** (spike tool) runs the same export plus the PROBE section between FACTIONS and END; its BEGIN has `mode` 1.

Both run a scripted GUI (`curia_run_advisor`, `curia_run_probes`) with the player as root scope. The first button sits at
the position the M2 stub proved clear of the HUD; the second is to its right. Both were seen on screen, clickable and clear of the HUD in M3 (Fullscreen); session 2 also clicked both in an Ironman game.

## Files
```
descriptor.mod                                     name, version 0.2.1, tags Utilities, supported_version 1.20.* (no BOM)
.metadata/metadata.json                            launcher metadata, same name/version/supported version, id "curia"
gui/curia_widget.gui                               the widget with the two buttons
gui/scripted_widgets/curia_widgets.txt             registers the widget (no vanilla GUI file overridden)
common/scripted_guis/curia_scripted_guis.txt       curia_run_advisor, curia_run_probes (scope character)
common/scripted_effects/curia_export.txt           the export, one effect per section, counting passes before the headers
common/scripted_effects/curia_probes.txt           the probe section: one effect per group, plus the helper curia_probe_faction_fields
common/script_values/curia_values.txt              caps, frame counters, the values the export prints, probe-only values
common/on_action/curia_on_actions.txt              the game-start record CURIA1|0|LOAD|0.2.1
localization/english/curia_l_english.yml           button texts and every record form (UTF-8 BOM, l_english:)
```
Script and localization files carry a UTF-8 BOM and tabs, like the game's own; `python3 tools/ci/lint_mod.py` checks that and the format invariants below.

## Install and remove (maintainer, macOS; quit CK3 completely first)
```sh
tools/m3/install_mod.sh                        # installs into ~/Documents/Paradox Interactive/Crusader Kings III/mod
tools/m3/install_mod.sh --variant descriptor   # T11 (deferred to M7): descriptor.mod only; --variant metadata: .metadata/metadata.json only
tools/m3/install_mod.sh --supported-version 1.19.*   # T11 (deferred to M7): rewrites supported_version in the installed copy only
tools/m3/install_mod.sh --uninstall            # removes <mod folder>/curia and curia.mod
```
Options `--mod-dir <dir>`, `--dry-run` and `--force` exist; `--help` lists them. The script refuses to run while CK3 runs and
never modifies this repository copy. Normal launch only (never debug mode), new game: the `LOAD` line is written at the
start of a new game; loading an existing save does not print it (M3), and one Ironman run in session 2 printed it twice, 40 s apart (cause not established), so never rely on exactly one `LOAD` line.

## Records
Every record starts with `CURIA1|<snap>|`; `<snap>` is a counter that goes up by one per export, `LOAD` has snap 0. Fields are
separated by `|`, there is no escaping, and **every record has at most one text field, always the last**, so a game string with a
pipe in it cannot move another field. The fields, their ranges and the frame rules are in `docs/wire-format.md` sections 2 to 4;
this table is only the shape the mod writes (`<n>` numbers items inside a section from 1).

| Record | Fields after `<snap>\|` |
|---|---|
| `BEGIN` | `BEGIN\|<mode 0/1>\|<mod_version>\|<year>\|<month>\|<day>\|<ruler id>\|<ruler name>` |
| `FIN` | `FIN\|<gold>\|<net monthly balance in tenths of a gold piece>` (+7.8 prints as 78) |
| `HEIR` | `HEIR\|none`, or `HEIR\|<id>\|<opinion of the heir's liege>\|<has liege 0/1>\|<landed 0/1>\|<name>` |
| `VASSALS`, `VASSAL` | `VASSALS\|<total>\|<listed>`, then `VASSAL\|<rank>\|<id>\|<gold>\|<opinion of liege>\|<military strength>\|<name>` (top N by strength, N = `curia_max_vassals`, default 10) |
| `WARS`, `WAR` | `WARS\|<total>` (wars are not capped), then three records per war with the same `<n>`: `WAR\|<n>\|NAME\|<war name>`, `WAR\|<n>\|ATTACKER\|<id>\|<name>`, `WAR\|<n>\|DEFENDER\|<id>\|<name>` |
| `CLAIMS`, `CLAIM` | `CLAIMS\|<explicit>\|<implicit>\|<listed>`, then `CLAIM\|<n>\|strong or weak\|<tier>\|<title>` (explicit claims only, highest tier first, at most `curia_max_claims`, default 10; `strong` = pressed, `weak` = unpressed) |
| `FACTIONS`, `FACTION` | `FACTIONS\|<against total>\|<member 0/1>\|<listed>`, then per faction (at most `curia_max_factions` = 5 against the ruler, strongest by power first, plus the faction the ruler has joined) five records with the same `<n>`: `INFO\|against or member\|<type 1..7>\|<power>\|<threshold>\|<members>`, `LEADER`, `CLAIMANT`, `TITLE`, `TARGET`, each either `none` or `<id>\|<name>` (`TITLE`: `<tier>\|<title>`). `CLAIMANT` and `TITLE` are written only for a claimant faction (type 3), `none` otherwise. Faction names are not exported |
| `PROBE` | `PROBE\|<name>\|<value>...`, probe button only; an unknown record type to the format (ignored, counted) |
| `END` | `END\|<count>`: the number of records in the frame, BEGIN and END included |

A war is three records because the war name is only reachable from the war scope and the side from its own scope (the proven
forms cannot be combined in one line). `ATTACKER` and `DEFENDER` are the war's primary sides and carry ids; the ruler may only be
an ally, so the app compares them with the `BEGIN` ruler id. The engine puts about 110 bytes in front of every line (script path, effect name), so the parser finds `CURIA1|`
anywhere in the line.

Names come from the `NoTooltip` forms and include titles (a ruler prints like "King Test Ruler One of Test Realm"); numbers are plain
integers through `[...|0]`. A debug_log call prints one localization key, so every form (strong and weak claims, against and
member factions, each `none` form) has its own key in `localization/english/curia_l_english.yml`.

### How the invariants are kept
- **`listed` and `total` are counted, not computed.** Before a header is written, the same iterator (same `max`, `limit`, order) runs
  once without output and counts into a global variable (`curia_vassals_listed`, `curia_wars_total`, `curia_claims_listed`,
  `curia_factions_listed`); a script value reads it for the header, then the writing pass runs. The two passes of a section keep
  identical parameters.
- **`END` count.** `BEGIN` sets `curia_export_lines` to 1; every other `debug_log` is preceded by `change_global_variable = {
  name = curia_export_lines add = 1 }`, and the lint checks every call, probes included.
- **Every global variable that is set is read by a script value** in `curia_values.txt` (otherwise the engine reports it when the
  mod loads), and every counter is reset in `curia_export_begin`.
- **Visibility rule (owner decision D15):** only data the player could see in the game's UI is exported.

### The lint (`tools/ci/lint_mod.py`, tests `tools/ci/test_lint_mod.py`)
Besides the structure checks (BOM, LF, `curia_` names) the lint checks for this mod: the version in `descriptor.mod`, the `LOAD`
literal, the `BEGIN` key and `.metadata/metadata.json` agree; every `debug_log` names a defined key and every `curia_line_*` /
`curia_probe_*` key is used; every `debug_log` directly follows the counter increment and no inline literal other than `LOAD`
exists; every `curia_line_*` key fits the record table of the spec (field count and kinds, one text expression and only last, only
the integer and text expressions that ran, no pipe or markup inside an expression, defined script values) and every form of the
table has a key; the counters and header totals read the script value the spec names (a swapped `n` or total is caught); every
global that is set (also in a multi-line block) is read, and every counter is reset in `curia_export_begin`; per effect, in
every script file, the writes of `curia_export_lines` pair one to one with the `debug_log` calls; `curia_export_run` calls the
sections in the order of the spec; the counting and the writing pass of each ordered iterator have identical parameters; every
counting global is changed somewhere. Localization keys are indented by exactly one space.

## Forms: ran in the game versus new in 0.2.0
The pre-freeze check of 2026-10-11 ran 0.2.0 (final `errcount` 0); everything below ran there except the income, which was the wrong quantity (see the next list); 0.2.1 changes only that value and adds probes.

Ran in M0/M0b/M3 (`docs/spikes/`): `[THIS.GetCharacter.GetNameNoTooltip]`, `[THIS.GetCharacter.GetGold|0]`,
`[THIS.GetCharacter.GetID]`, `[THIS.War.GetName]`, `[THIS.Title.GetNameNoTierNoTooltip]`, `[THIS.ScriptValue('x')|0]`,
`[EmptyScope.ScriptValue('x')|0]` (also over `global_var:`), the script value `"opinion(liege)"`, the iterators `player_heir`,
`ordered_vassal` (`max`, `check_range_bounds = no`, `order_by = current_military_strength`), `every_character_war` with
`primary_attacker` / `primary_defender`, `ordered_claim` (unfiltered, and with `explicit = all` / `pressed = all`), `every_claim` with
the `explicit` / `pressed` filters, `every_targeting_faction`, `joined_faction`, `faction_leader`, `faction_target`, `faction_is_type`,
`faction_power`, `faction_power_threshold`, `every_faction_member`, and `save_temporary_scope_as` with `scope:` and `prev` inside a claim
iterator (the claim probe).

New in 0.2.0 (the list of `docs/wire-format.md` section 8 is the authority; the net income form of 0.2.1 has not run):
- the date as the script values `current_year`, `current_month` and `current_day` (`current_year` is used as a value in the game's own
  files; month and day appear there only as comparisons, but ck3-tiger accepts them in a script value; the data functions
  `[GetCurrentDate.GetYear]`, `GetMonthOfYear` and `GetDayOfMonth` exist as the fallback);
- income in tenths: since 0.2.1 the net monthly balance, the script value `monthly_character_income` minus `monthly_character_expenses`, multiplied by 10 and rounded once (0.2.0 used the income before expenses, which the pre-freeze check showed to be 9.2 against a top bar of +7.8);
- `ordered_claim` with `explicit = yes` and a `limit` on `has_strong_claim_on` / `has_weak_claim_on` through a saved temporary scope;
- `ordered_targeting_faction` with `max`, `check_range_bounds = no` and `order_by = faction_power`;
- `special_character` and `special_title` with `has_special_character` / `has_special_title` and `exists`, inside a guard on
  `faction_is_type = claimant_faction`;
- the counting passes and the header values that read their globals; the `none` forms; the literal `mod_version` token in the `BEGIN` key.

## Probe section
The probe section sits between FACTIONS and END and only exists in the probe button's frame. Each line tests one form, so a failure shows up as an empty or 0 value on that line only. Groups, in emission order: ruler,
heir, strongest vassal, highest-tier claim, claim filters, claim detail, one war, factions that target the ruler, the
faction the ruler joined or leads. Status: the ruler, heir, vassal, claim and war rows ran in session 1 with sensible values
and no error. The rows marked **session 2** (claim filters, claim detail, all faction rows) ran on 2026-10-11 with no
`error.log` entry (`docs/spikes/m3-results.md`): the claim filters and `claim_detail` for the rulers of two games (and the
filters for two heirs), and the faction rows for three factions in two games (claimant, peasant and independence). What
they showed, in short: the unfiltered claim count equals the `explicit` = all, `pressed` = all filter; the one ruler claim
seen with `claim_detail` flags each time was explicit (weak in one game, strong in the other) and the heir's three claims
in the Ironman game were implicit; `faction_name` carries link markup; power was 1 off the Factions window in one
faction and 1 apart between its two forms in another; discontent was 0 in every faction; `can_press_demands` and
`dangerous` printed 0 or 1 without a meaning that could be established. Never seen in a run: the implicit flags at 1 on a
ruler's claim, a nonzero discontent, and faction types other than independence, claimant and peasant.

| Probe name | Form under test |
|---|---|
| `snap_id_this` | the snapshot-id script value read through `THIS` instead of `EmptyScope` |
| `ruler_id`, `heir_id`, `vassal_id` | `[THIS.GetCharacter.GetID]` |
| `ruler_first_name`, `heir_first_name`, `vassal_first_name` | `[THIS.GetCharacter.GetFirstNameNoTooltip]` (names without titles) |
| `ruler_income_monthly`, `ruler_income_yearly` | script values over `monthly_character_income`, `yearly_character_income` |
| `ruler_income_gross_tenths`, `ruler_expenses_tenths`, `ruler_net_tenths` | income before expenses, expenses and the exported net figure, each in tenths and rounded (0.2.1, for the income re-check) |
| `ruler_balance_tenths` | the game's own `monthly_character_balance` over 10, rounded: **a candidate**: the game's files use the name once, inside a scale block (`liege.monthly_character_balance`), never as the value of a named script value; remove the probe if it raises an `error.log` entry |
| `ruler_strength`, `vassal_strength` | script value over `current_military_strength` |
| `ruler_realm_size` | script value over `realm_size` |
| `ruler_landless_ruler`, `heir_landless_ruler` | script value over the `is_landless_ruler` trigger |
| `vassal_landed` | the `is_landed` flag value in the vassal scope (the same value the HEIR line uses) |
| `vassal_count_trigger`, `vassal_count_iterator`, `vassal_count_below` | `vassal_count`; `every_vassal` counting; `every_vassal_or_below` counting |
| `ruler_claim_count`, `heir_claim_count`, `vassal_claim_count` | script value with `every_claim` counting |
| `claim_tier` | script value over `tier` in the title scope |
| **session 2** `ruler_claims_e<x>_p<y>`, `heir_claims_e<x>_p<y>` | claim counts for every `explicit` / `pressed` pair (`x`, `y` = `yes`, `no`, `all`; nine lines each, the heir's only if there is an heir), to compare with `ruler_claim_count` and the game's character window |
| **session 2** `claim_detail` | one line per claim (highest tier first, at most `curia_max_claims`), nine fields after the snapshot number (`PROBE` plus eight): `claim_detail`, index, title, explicit-strong, explicit-weak, implicit-strong, implicit-weak (0/1 from `has_strong_claim_on`, `has_weak_claim_on`, `has_strong_implicit_claim_on`, `has_weak_implicit_claim_on`), tier; `PROBE\|claim_detail\|none` when there is no claim |
| `war_score`, `war_start_date`, `war_enemy` | `[THIS.War.GetWarScore\|0]`, `[THIS.War.GetStartDate.GetString]`, `[THIS.War.GetPrimaryPlayerEnemy.GetNameNoTooltip]` |
| **session 2** `faction_source` | `targeting` (factions that target the ruler) or `joined` (the faction behind the `joined_faction` link); `faction_joined\|none` / `faction\|none` when absent |
| **session 2** `faction_type_code`, `faction_power_threshold`, `faction_at_war`, `faction_can_press_demands`, `faction_dangerous_df` | script values (type through `faction_is_type`, threshold, at-war, can-press-demands flags) and `[Select_CString(THIS.Faction.IsDangerous,'1','0')]` (the bool function through `Select_CString`) |
| **session 2** `faction_name` | `[THIS.Faction.GetNameNoTooltip]`; the output still contains link markup (control byte 0x15), seen on the longest CURIA line so far (321 bytes with the engine prefix) |
| **session 2** `faction_power_df`, `faction_discontent_df` | `[THIS.Faction.GetPower\|0]`, `[THIS.Faction.GetDiscontent\|2]` |
| **session 2** `faction_power_sv`, `faction_discontent_sv` | script values over `faction_power`, `faction_discontent` in the faction scope |
| **session 2** `faction_member_count` | script value with `every_faction_member` counting |
| **session 2** `faction_leader`, `faction_target` | the `faction_leader` / `faction_target` scope links (guarded with `exists`; `PROBE\|faction_leader\|none` when absent) |

Non-ASCII name handling needs no code: it is observed from real names in the capture.

To turn probes off: remove `curia_export_probes = yes` from `curia_export_run`, or one group line from `curia_export_probes`,
or one `change_global_variable` + `debug_log` pair (all in `common/scripted_effects/`). The scope links that could fail
(`faction_leader`, `faction_target`, `joined_faction`, `player_heir`, vassals, wars, claims) are guarded with `exists` or
`any_`, and the rest are data functions and script values that print an empty value or 0 when they do not resolve. The
session 2 candidates that could have raised a script error (`save_temporary_scope_as` plus `scope:curia_probe_claimant` and
`prev` inside the `claim_detail` effect; `explicit` / `pressed` on `ordered_claim`, including the pairs with `no`;
`faction_is_type` and `faction_power_threshold` inside faction-scope script values; all `THIS.Faction...` forms) ran in
session 2 with no `error.log` entry matching Curia; they are no longer open risks for the cases run. Still not covered
by any run: a ruler with an implicit claim (the implicit-flag cases of `claim_detail`), a nonzero discontent, faction
types other than independence, claimant and peasant, and the `special_character` / `special_title` forms of the
CLAIMANT and TITLE records (they are in the export, guarded for type 3, and have never run in the game). `faction_*` output is not capped (about 16 lines per faction). A data function that does not exist would be reported when the localization loads
(`error.log`); ck3-tiger found none.

## Spike limits
- The two HUD buttons span x 570 to 866 (bottom|left anchor) and were checked clear of the HUD only in the owner's 2056-point-wide Fullscreen view. The vanilla bottom-right block is 630 wide, so on a UI narrower than about 1,400 units the probe button can overlap it; the shipped mod anchors relative to the bottom-left block instead.
