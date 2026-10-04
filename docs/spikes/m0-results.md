# Milestone 0 results

Run by the owner on 2026-10-04: macOS 26.5.1 (arm64), CK3 1.20.0.3, clean playset with only the stub mod `mod/curia-m0/`. Raw outputs: the owner's `~/curia_m0_results/` (not committed; they contain engine log text). Runs: A (normal launch, Ironman off), A2 (same session, later), B (same session, Ironman on), C (debug-mode launch from the launcher's Game Settings). Run D (console `help`) was also done.

## Verdict against the M0 decision table

| Test | Result | Verdict |
|---|---|---|
| T0 log location | `~/Documents/Paradox Interactive/Crusader Kings III/logs/debug.log` exists after launch; the stub mod was confirmed loaded (the mod appears as enabled and mounted in the same log) | **pass** |
| T1 normal launch | `debug_log` writes to `debug.log` with **no** `-debug_mode` (run A: `S1`..`S8`; `Q1`..`Q8` first appeared 11 in-game days after start, then every quarter) | **pass** |
| T1 Ironman | lines appear in an Ironman game (run B) | **pass** (NFR-6 Ironman compatibility holds) |
| T1 debug-mode launch | lines also appear (run C) | n/a (not needed) |
| T2 achievements | normal launch: setup screen, pause-menu icon and Game Rules window all say "Available", also after the hook lines fired and in the Ironman game. **Debug-mode launch (run C): "Not available" immediately, with no console command and no warning mark** (contradicting the game's own text, which suggested only a warning). The cause was not isolated (a different bookmark was used, rules unknown) | **pass** for normal launch; **debug-mode launch is not achievement-safe** |
| T2c console | key is `` ` ``; `help` worked; icon stayed "Not available" (it already was, so timing is not learned) | inconclusive, not needed |
| T6 brackets/values | **partial**: loc expressions **are** evaluated, numbers resolve, but only some scope forms (below) | **partial: ask before proceeding** (protocol rule) |

Outcome: the approach is feasible. A normal launch with the mod keeps achievements available, in Ironman too, so "achievement-compatible, no `-debug_mode`" is confirmed as the design goal. The one open point is T6, below.

## T6 detail (what resolved)

| Form | In game-start hook (root none, inside `every_player`) | In quarterly hook (root = the ruler) |
|---|---|---|
| plain string | resolves | resolves |
| quoted string with brackets (`S2`/`Q2`) | **is evaluated as loc** (output shows link markup with an invalid character id, empty name); the name was empty because the scope form below did not resolve | evaluated; `[ROOT.Char.GetName]` gave an empty name |
| `[SCOPE.sC('saved_scope').GetNameNoTooltip]` | empty | not tested |
| `[ROOT.Char.GetNameNoTooltip]`, `[ROOT.Char.GetGold|0]`, `[ROOT.Var('x').GetValue]` | not tested | all **empty** |
| `[THIS.GetCharacter.GetNameNoTooltip]` (the exact form vanilla uses) | **resolves** (character name) | **resolves** |
| `[EmptyScope.ScriptValue('const')]` | `1234` | `1234` |
| `[SCOPE.ScriptValue('gold_value')|0]` (a script value `value = gold`) | game error "value of wrong type, got none" (SCOPE was none here), printed `0` | **resolves** (`1033`, later `1236`) |
| `[SCOPE.sC('saved_scope').MakeScope.Var('x').GetValue]`, `[SCOPE.sC('saved_scope').GetGold|0]` | printed `0` (not resolved) | not tested |

Reading: inside `debug_log`, `SCOPE` is the effect's **top** scope (character in the quarterly hook, none in the game-start hook even inside `every_player`), `THIS` is the **current** scope and works, while `ROOT.*` forms and saved-scope (`sC`) forms came back empty. Numbers print as plain integers; with `|0` also plain integers; `GetName` (with tooltip) emits link markup such as `ONCLICK:CHARACTER,<id> TOOLTIP:CHARACTER,<id> L <name>!!!`, so use the `NoTooltip` variants. Quoted strings with brackets work the same as localization keys, so the exporter does not need loc files for each line.

Not yet tested, needed for the exporter: `THIS`-based gold (`[THIS.GetCharacter.GetGold|0]`), variables via `THIS`, script values via `THIS.ScriptValue`, and values read from iterator scopes (vassals, heir). These are small variants of what worked and can be pinned in the first M3 iteration.

## Other facts learned

- **Line shape** (verbatim structure, our marker in the message): `[HH:MM:SS][D][jomini_effect_impl.cpp:450]: file: <script path> line: <n> (<on_action or effect name>): <message>`. The engine adds about 110 bytes of prefix to every `debug_log` call (8 test lines = 1,332 bytes, about 166 bytes each). The prefix contains our script file name and on_action/effect name, so the parser must locate the marker inside the line, never assume a column. Pipes are preserved.
- `debug_log_date = yes` writes a **separate** unmarked line (`DATE: yes - <year>.<month>.<day>`) right after the marker line; the in-game date is therefore available as a separate line or can be exported as a value.
- All test lines went to `debug.log` only; `error.log` got none.
- `debug.log` is recreated at each game launch (run C's fresh launch started at about 1.0 MB with only its own lines); within one launch lines accumulate across new games. Startup of a launch was about 1.0–1.4 MB; **starting a second new game added about 1.0 MB**, so the observed baseline is roughly 1 MB of `debug.log` per new game plus about 0.1 MB in `error.log` (relevant to the unconfirmed 17MB cap; not a measurement of it).
- The game reports at mod load, in `error.log`, that a variable is "set but never used" (our `set_variable` for the T6 test); local use in loc "doesn't count". The real mod should not set variables it does not read in script, since FR-MOD-7 wants a clean `error.log`.
- Flush latency was **not** measured (the script ran 38 s after the lines were written). It stays an M3 test (T3).
- Not recorded by the owner: the Load Game detail-pane achievements text (not needed for the verdict).

## Milestone 0b (same day; chosen by the owner)

Run by the owner as one launch with the richer stub (`m0b-checklist.md`): King Harold II Godwineson of England, start 15 September 1066 (10 vassals; heir Prince Godwine, landless, with one claim of his own; one claim of the ruler, Devon; two active wars, Norway and Normandy; no faction against him). Helper runs `M0B` (13 October 1066, first pulse) and `M0B2` (31 January 1067, second pulse). Stub: `mod/curia-m0/` (two on_action files, script values, loc).

### Value forms that resolve (all inside `debug_log` strings; same form works as a quoted string)

| Probe | Form | Result |
|---|---|---|
| Ruler name | `[THIS.GetCharacter.GetNameNoTooltip]`, `[THIS.Char.GetNameNoTooltip]` | both resolve ("King Harold of England": title and "of <title>" are included) |
| Ruler gold | `[THIS.GetCharacter.GetGold|0]` | resolves (375, then 413 at the second pulse) |
| Variable | `[THIS.Var('x').GetValue]`, `[THIS.GetCharacter.MakeScope.Var('x').GetValue]` | both resolve (42) |
| Script value | `[THIS.ScriptValue('x')|0]`, `[SCOPE.ScriptValue('x')|0]` at the top scope | both resolve (gold 375) |
| Primary title | `[THIS.GetCharacter.GetPrimaryTitle.GetNameNoTierNoTooltip]` | resolves ("England", "Northumbria", "Mercia") |
| Heir | `player_heir = { … }`; name, liege, opinion | resolves ("Godwine": first name only because he holds no title) |
| Vassals | `ordered_vassal = { max = 2 order_by = current_military_strength … }`; name, title, gold | resolves |
| Wars | `every_character_war`; `[THIS.War.GetName]`; `primary_attacker = {…}` and `primary_defender = {…}` | resolve ("Norwegian Invasion of England", attacker/defender names) |
| Claims | `random_claim`; `[THIS.Title.GetNameNoTierNoTooltip]` | resolves ("Devon" for the ruler, "Middle Seaxe" for the heir) |
| **Opinion of the ruler** (vassal and heir) | script value `value = "opinion(liege)"` read as `[THIS.ScriptValue('x')|0]` | **resolves** (-37 and -87 for the two vassals, 83 for the heir) |
| Opinion via the court owner (heir) | script value `value = "opinion(liege_or_court_owner)"` | resolves (83, same as `liege`: the heir had a liege link) |
| Opinion precomputed | `set_variable = { name = x value = <script value> }` inside the iterated scope, read as `[THIS.GetCharacter.MakeScope.Var('x').GetValue]` | **resolves** (same numbers) |
| Opinion via a saved scope inside a script value (`opinion(scope:saved)`) | | **does not work**: prints 0 and logs "Undefined event target" errors in `error.log` |

Not tested: faction-scope forms (`THIS.Faction…`, no faction against this ruler), so the `F1`..`F3` probes printed only "no factions". Names include the title prefix and "of <title>"; the exporter will likely want first names or ids (check in M3).

### Line length and size
- A single `debug_log` line of **8,000 characters survived intact** (8,135 characters with the engine prefix, `_END` tag present); 500 and 2,000 too. The real maximum is beyond 8,000; no truncation seen.
- A full pulse of 47 short probe lines cost about 7.7 KB (about 164 bytes per line, most of it the ~110-byte prefix); with the three long lines the pulse was 18.4 KB.
- Startup `debug.log` this launch: about 1.0 MB. The first pulse came 28 in-game days after the start (13 October for a 15 September start): the 11 days seen earlier were character-specific (the pulse is relative to each character's birthday).
- **Flush latency still unmeasured**: the file's modification time equalled the pulse's second, but the stub's own script errors (`error`-level lines force a flush, per the log settings) confound it. Measure in M3 with an error-free export.

### Verdict
T6 is **resolved**: names, gold, variables, script values, opinions of the ruler, and probes from heir, vassals, wars and claims all resolve with `THIS`-based forms or script values. The M0 acceptance criteria are met (D9 not triggered). Per M0 criterion 3 nothing starts until the owner confirms the go-ahead for M1. 
### Cleanup status
The stub mod was removed, debug mode was turned off in the launcher and the usual playset is active again. Cleanup was only partial: the `curia_m0_*` throwaway saves were kept, and the usual campaign was not re-checked (its achievements were already disabled before M0, so nothing was lost).
