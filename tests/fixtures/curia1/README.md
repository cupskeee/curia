# CURIA1 fixtures

Test input for the bridge core (`app/src/bridge/`, specified in `docs/wire-format.md`). Each case is a
pair of files:

- `<name>.log`: the input, one **bare** record per line (it starts with `CURIA1|`), plus directives;
- `<name>.events.json`: the golden output, the list of events the bridge must produce for it.

All names in the fixtures are artificial ("Test Ruler One", "Test Vassal Three"). No engine log
line is stored here: the harness adds a synthetic engine-style prefix of about 120 bytes to every
bare record at test time (the app's own fixture loader in `app/tests/bridge_test_support.cpp`, and
later `tools/fake_ck3`), because the real prefix is not part of the format and must never be
committed. A second implementation may use any prefix of **at least 1 byte** that does not contain
the marker (a marker at offset 0 is not a record, so an empty prefix would make every record fail).
No golden depends on the prefix length: the records that matter for the 1,024-byte and 4,096-byte
rules either are cut inside the record, or sit on a `!raw` line without a prefix.

## `.log` format

One line per entry; lines end in `\n` (a `\r` before it is ignored).

| Line | Meaning |
|---|---|
| `# text` | comment, skipped |
| (empty) | skipped |
| `CURIA1\|...` | a bare record: written to the log as `<synthetic prefix><line>\n` |
| `!noise <text>` | a non-Curia line, written verbatim (`<text>\n`); a bare `!noise` (no trailing space, which the editorconfig would strip) is an empty line |
| `!raw <text>` | written verbatim, with no prefix (a marker at offset 0 is not a record); a bare `!raw` is an empty line; this is how a line gets a lead-in of exact length before the marker |
| `!truncate` | the harness truncates the log to zero length (the file shrank: the unterminated tail is dropped and an open frame is abandoned) |

Anything else is an error. A fixture is read in order; the events are the concatenation of the
events of every step. The tests feed the bytes in one piece, in fixed chunk sizes (1, 2, 3, 7, 64
bytes) and in random chunks: the events must not depend on the chunking.

A text file can hold long lines (`overlong_line`) and many records (`damaged_too_large`,
`frame_512_records`). What it cannot hold is tested in code instead of in a fixture: invalid UTF-8, a
NUL byte, a lone `\r` before the line end, an unterminated tail, and chunk-split sweeps. The one
fixture with control bytes is `markup_sample.log`, which contains the raw byte `0x15` (the engine's
link markup).

## `.events.json` format

A JSON array of events; the keys are documented in `app/src/bridge/frame_json.h`:
`frame_complete` (with the full frame), `frame_damaged` (snap, reason, detail),
`unsupported_format` (snap, leading_fields) and `diagnostic` (snap, reason, detail). The `detail`
strings are for humans and may be reworded; a fake CK3 should compare the event kinds, reasons and
frames.

To regenerate after an intended change: `CURIA_UPDATE_GOLDEN=1 ./curia_tests -tc="fixtures: golden outputs"`
from the build directory, then read the diff against the specification before committing it.

## The fixtures

| File | What it shows |
|---|---|
| `example_frame` | the example frame of the specification |
| `minimal_frame` | BEGIN and END only; no section exported |
| `sections_full` | every section with items, a negative income, a war the ruler only attacks in and one it is an ally in |
| `sections_empty` | every section present and empty |
| `negative_zero` | `-0` in the signed fields |
| `none_forms` | HEIR `none`, and the `none` form of LEADER, CLAIMANT, TITLE, TARGET |
| `factions_all_types` | seven factions, types 1 to 7, both roles |
| `war_roles` | the ruler as attacker, as defender, and not in the war's id list (an ally) |
| `claims_kinds` | strong and weak claims over the tiers |
| `damaged_invalid_record`, `damaged_snap_mismatch` (higher and lower snaps, known and unknown types, END), `damaged_count_mismatch`, `damaged_group_count`, `damaged_order`, `damaged_incomplete`, `damaged_truncated`, `damaged_too_large` (the 513th record is the END) | one Damaged reason each, every file ends with a good frame |
| `overlong_line` | physical lines over 4,096 bytes: a long name (shortened), a short record at the end of the head (shortened because the line is overlong), a complete FIN whose line is overlong (invalid) |
| `oversize_no_text` | a known record without a text field that is over 1,024 bytes is invalid |
| `faction_other_type` | CLAIMANT and TITLE values in a faction that is not type 3 are valid records but are not kept |
| `frame_512_records` | the largest frame that is still allowed |
| `noise_interleaved` | lines without the marker inside a frame |
| `name_with_pipe` | game strings containing `\|` and even a marker |
| `markup_sample` | the worked hygiene sample of the specification, and more markup, also in the faction LEADER, CLAIMANT, TITLE and TARGET records |
| `unknown_types` | unknown line types and sub-kinds, counted but ignored |
| `probe_in_frame` | PROBE records are an unknown type |
| `load_records` | LOAD (snap 0), repeated, also inside a frame |
| `snap_falling_repeated` | snapshot numbers that fall and repeat |
| `old_layout_begin` | the spike layout of the mod: an unsupported-format report |
| `oversize_name` | names over 1,024 bytes, shortened with the frame kept |
| `snap0_records` | invalid records with snap 0 never touch a frame |
| `double_end` | an END after a complete frame, and before any BEGIN |
| `marker_offset0` | the marker at offset 0 is not a record |
| `invalid_records_idle` | the worked invalid records of the specification, between frames |
| `valid_notable` | the valid-and-notable records of the specification |
