# Curia wire format `CURIA1` (DRAFT for the owner's checkpoint, not frozen)

Status: **draft, 2026-10-11. Nothing here is frozen or decided.** The owner reviews this file at the wire-format checkpoint of Milestone 3 (Phase 2); it becomes the frozen `CURIA1` specification only after the owner approves it and the pre-freeze in-game check (section 9) has passed. Until then the mod's current spike output is unchanged and this file describes the *proposed* output.

What it is: the contract between the mod (`mod/curia/`, writes) and the app (`app/`, reads) through CK3's `debug.log`. Evidence is in `docs/spikes/m3-results.md` (session 1, the PR #11 re-check and the session of 2026-10-11, cited as "session 2" in this file). The engine facts are in `docs/research-notes.md` §2.14 and §2.16.

## Decisions asked of the owner

Each item is a proposal. None is decided. The reasons are in the section named. Sub-rows (a, b, ...) can be accepted or rejected one by one.

| # | Proposal | Section |
|---|---|---|
| W1 | Every line has at most one free-text field and it is last; no escaping. A pipe or markup in a game string cannot move another field | 2 |
| W2a | The title text is dropped from `BEGIN` and `VASSAL` (the ruler's printed name was seen to carry the title in M0; for vassals, leaders and targets this is unverified and is part of the pre-freeze check; if a vassal's name lacks it, the title comes back as its own record type) | 4, 9 |
| W2b | Ids are added (heir, vassals, war sides, faction characters) so characters can be matched across records | 4 |
| W3a | Section header lines with totals (`VASSALS`, `WARS`, `CLAIMS`, `FACTIONS`) replace the old `none` lines, so a skipped section is told apart from an empty one | 3, 4 |
| W3b | The headers carry a `listed` field (alternative: drop it and let the app count the item lines; `total` and the `END` count already catch truncation) | 4 |
| W3c | Group and count rules are normative and checked by the parser (table in section 3) | 3 |
| W4 | The date is three integers plus a `mod_version` token in `BEGIN`; the localized date string is dropped | 4 |
| W5 | Claims: `CLAIM` lines list **explicit** claims only, each `strong` or `weak`; implicit claims are a count (recommended) | 6 |
| W6a | Factions carry no name | 7 |
| W6b | Faction data: type, power, threshold, member count, and `LEADER`, `CLAIMANT`, `TARGET` records | 4, 7 |
| W6c | `CLAIMANT` is `special_character`, written by the mod only for type 3, and read by the app as a claimant only for type 3 | 7 |
| W6d | At most 5 factions against the ruler (strongest by power, through an ordered iterator that has never run) plus the ruler's own | 7 |
| W6e | `discontent`, `can_press_demands`, `dangerous` and `at_war` stay out of `CURIA1` until their meaning is established (alternative for `discontent`: export its script-value integer; its scale is unknown, it was 0 in all three factions seen) | 7 |
| W6f | Faction type codes 1 to 7 (section 4), with 7 absorbing any type the mod does not know | 4 |
| W7 | One form per field: numbers come from script values as integers (power, income, strength), consumers treat them as approximate | 2, 7 |
| W8 | Wars are not capped; `WARS` gives the total, and frames above the 512-record limit are lost (alternative: cap at 5, with `total` and `listed`). `ATTACKER` and `DEFENDER` carry ids so the app can tell which side the ruler is on, or that the ruler is only an ally | 3, 4 |
| W9 | Additive-only compatibility inside `CURIA1`: new data is a new line type or sub-kind, fields are never added to a type, enum value sets are closed | 2, 10 |
| W10 | Arrival order decides which frame is current; the snapshot number only matches records inside a frame; a date that goes back is a hint of a reload; the frame lifecycle is the state machine of section 3 | 3 |
| W11 | Committed fixtures hold bare `CURIA1\|...` records with plainly artificial names; the engine-style prefix is added at test time | 11 |
| W12 | The mod is changed to emit this format first (list in section 9), then a short in-game check covers every **new** row of section 8; only then is the format declared frozen | 9 |

How this maps to the owner's four requests of 2026-10-11: (1) markup or pipes cannot inject a field: W1 and W6a (sections 2 and 7); (2) implicit versus explicit claims: W5 (section 6); (3) one form per field: W7 and W6e (sections 2 and 7); (4) a claimant field: W6c (section 7).

Not wire-format, listed because it was proposed earlier: a "pause the game first" hint in the overlay (the game keeps running while the player types) is an app behaviour for M4, not part of this file. Open questions that need the owner are collected in section 12.

## 1. Transport

- The mod writes one `debug_log` call per record. CK3 appends each as one line of `debug.log` (UTF-8, `\n` terminated).
- **Only a line terminated by `\n` is a record.** An unterminated tail is held back by the watcher and never parsed (a torn read would otherwise give a valid record with a wrong number); after a truncation of the file it is discarded. Exactly one `\r` immediately before the `\n` is removed. A physical line longer than **4,096 bytes** without a `\n` is skipped up to the next `\n`.
- The engine puts its own prefix in front of every such line (observed 119 to 124 bytes in session 1, 119 to 131 bytes in session 2: time, level, source, script path, effect name). The prefix is not part of the format and its shape is not relied on. The **record** is the text from the first occurrence of `CURIA1|` to the end of the line. A marker at offset 0 (no engine prefix at all) is not a record: the engine always writes a prefix, and this stops the tail of a game string that contained a newline from forging a record.
- Lines without `CURIA1|` are ignored. Non-Curia lines may sit between two Curia lines of one frame (the engine's multi-line error entries did in M3); they are ignored and not counted. Whether an error entry can quote the marker is not known (section 9).
- The log is truncated in place at game launch (same inode, size back to 0). That is the watcher's concern (`architecture.md` §5), not the format's: after a truncation any open frame is abandoned (section 3).
- Records are small: the longest Curia line seen was 199 bytes in session 1 and 321 bytes in session 2 (a faction name line of the probe export; the prefix is included in both). A record longer than **1,024 bytes**, measured from the first `C` of the marker to the end of the line, without the `\r` and `\n`, is invalid.
- Threat model: frames are not authenticated. Anyone who can write to `debug.log` can forge records. A hostile game string can at most change its own text field (a newline in it is the one exception and is covered by the marker rule above); it can never alter another field (section 2).

## 2. Record syntax

```
CURIA1|<snap>|<TYPE>|<field>|<field>|...
```

- Fields are separated by `|`. There is **no escaping**: the mod cannot escape game text.
- `<snap>` is an int. `0` is reserved for records outside a frame (`LOAD`); frames use 1 and up.
- `<TYPE>` matches `[A-Z][A-Z0-9_]*`. A type outside that pattern is an invalid record. A type that matches but is not in the table of section 4 is an **unknown type**: ignored, but counted and snap-checked. Types with a sub-kind (`FACTION`, `WAR`) use the first word after `<n>` as part of the type key; an unknown sub-kind is an unknown type.
- Field kinds:
  - **int**: `0` or optional `-` then `[1-9][0-9]{0,9}`; `-0` is accepted and read as 0 (the game's rounding of a small negative number may print it; section 9). The value must fit int32. A lone `-`, a `+` sign, leading zeros, grouping separators, decimals and non-ASCII digits are invalid. Counts, `snap`, `rank`, `n`, ids and `mode` are not negative; `snap` is at least 1 in a frame; flags are 0 or 1; `tier` is 1 to 5; `type` is 1 to 7; `year` is 0 to 9999; `month` 1 to 12 and `day` 1 to 31 if the check confirms 1-based values (section 9). Snap equality is numeric on this canonical form. Out of range is invalid.
  - **enum**: one of the listed ASCII words, case-sensitive. The value sets are **closed** for the life of `CURIA1` (section 10); an unknown word is invalid.
  - **token**: `[A-Za-z0-9._+-]{1,32}` (a version string).
  - **text**: any bytes after the fixed fields (UTF-8 expected).
  An empty field in a non-text position is invalid. Bytes below `0x20` or `0x7F` and above (anything not ASCII) are only allowed in a text field; there is no trimming outside text.
- **Every line type has at most one text field and it is always the last field.** A text-bearing record has exactly its fixed fields, then a mandatory `|`, then the text, which may be empty (a game string can be empty, or emptied by hygiene) and may contain `|`. A record without text has exactly its fields. A game string containing `|` can therefore never move or change another field.
- A line type that allows `none` (`HEIR`, `LEADER`, `CLAIMANT`, `TARGET`) has two forms, the `none` form and the full form; the `none` form has no bytes after the word `none` (a trailing `|` or text is invalid).
- A record that breaks any rule above, or whose marker is followed by a malformed header (`CURIA1|`, `CURIA1||BEGIN`, `CURIA1|abc|FOO`), is **invalid**: it is dropped and the open frame is damaged (section 3).

Worked invalid records (the fuzz seed corpus and the fixtures carry these):

| Record (after the marker) | Why |
|---|---|
| `7\|FIN\|312` | too few fields |
| `7\|FIN\|312\|9\|1` | too many fields, no text field on `FIN` |
| `7\|FIN\|312\|9\r` inside the line | `\r` not directly before `\n` |
| `7\|FIN\|007\|9` | leading zeros |
| `7\|FIN\|+5\|9` | plus sign |
| `7\|FIN\|\|9` | empty int |
| `7\|FIN\|1e3\|9` | not an int |
| `7\|END\|99999999999999999999` | does not fit int32 |
| `7\|HEIR\|1002\|84\|1\|0` | text delimiter missing |
| `7\|HEIR\|none\|` | bytes after `none` |
| `7\|FACTION\|1\|LEADER\|1004` | text delimiter missing |
| `7\|CLAIM\|1\|Weak\|4\|x` | enum is case-sensitive |
| `7\|BEGIN\|0\|1.0 0\|...` | token with a space |
| `7\|fin\|1\|2` | type outside the pattern |

Valid and notable: `7|HEIR|1002|84|1|0|` (empty name), `7|HEIR|none`, `7|FIN|-0|-0`.

- **Text hygiene** (applied by the app to every text field, as defence in depth; the format does not depend on it), in this order: (1) repair invalid UTF-8 (U+FFFD); (2) delete the two bytes `0x15 0x21`; (3) delete a byte `0x15` followed by `ONCLICK:`, `TOOLTIP:` or `L` together with the rest of that word up to and including the next U+0020 (only the tags seen are known; the word is assumed to be followed by a space, as in the sample); (4) delete every remaining `0x15` and every other code point below U+0020, U+007F, U+0080 to U+009F, U+2028, U+2029, U+202A to U+202E and U+2066 to U+2069; (5) collapse runs of U+0020 and trim U+0020. Worked sample (`^U` is the byte `0x15`, ids and names artificial): `Install ^UONCLICK:CHARACTER,1 ^UTOOLTIP:CHARACTER,1 ^UL Test Ruler^U!^U!^U! on the ^UONCLICK:TITLE,2 ^UTOOLTIP:LANDED_TITLE,2 ^UL; Test Realm^U!^U!^U! Throne` becomes `Install Test Ruler on the Test Realm Throne`. This sample is a fixture. Faction names are no longer exported (section 7); character and title names in the `NoTooltip` forms were clean in M3, so hygiene is expected to change nothing for them.
- Numbers never carry decimals, grouping separators or units. Values the game computes as fractions are rounded by the mod to an integer; consumers treat them as approximate (power and income were seen to differ from the UI by 1 or less, sections 7 and 8).
- Language: names, titles and war names are written in the player's game language. All structural words (types, enums) and numbers are language-independent. The mod ships English localization only; behaviour with other game languages is untested (section 9, open assumptions).

## 3. Frame

A frame is one export, started by one button click.

```
BEGIN ... first record of the frame
<sections, in the order of section 4>
END|<count> ... last record
```

**Section rules.** `BEGIN` and `END` appear exactly once. `FIN`, `HEIR`, `VASSALS`, `WARS`, `CLAIMS` and `FACTIONS` (each with its items) appear at most once, in the order of section 4; an absent section means "not exported", never "empty". The items of a section follow its header directly and precede the next header. Unknown-type records are ignored for these order and adjacency checks.

**Checked counts** (any violation damages the frame). The parser does not use the mod's caps (10 vassals, 10 claims, 5 factions); the caps are the mod's business.

| Header | Rule |
|---|---|
| `VASSALS` | `listed` equals the number of `VASSAL` records; their `rank` runs 1..`listed` in order; `listed` <= `total` |
| `WARS` | `total` equals the number of `WAR` groups; `n` runs 1..`total`; each group is `NAME`, `ATTACKER`, `DEFENDER` adjacent and in that order, exactly once each |
| `CLAIMS` | `listed` equals the number of `CLAIM` records; `n` runs 1..`listed`; `listed` <= `explicit` |
| `FACTIONS` | `listed` equals the number of `FACTION` groups; `n` runs 1..`listed`; each group is `INFO`, `LEADER`, `CLAIMANT`, `TARGET` adjacent, in that order, exactly once each; the number of groups with `role` `member` equals `member`; the number with `role` `against` is <= `against` |

**State machine of the frame assembler.** States: *Idle*, *Open(snap)*, *Damaged(snap)*.

| Event | Idle | Open / Damaged |
|---|---|---|
| valid `BEGIN`, snap >= 1 | open a frame | the open frame is abandoned (a Damaged event is emitted for it), a new frame opens |
| invalid `BEGIN` whose snap parses | report "unsupported mod format" with the leading fields | the open frame is abandoned as above, then report, then Idle |
| any other known type | ignored (the watcher may attach mid-frame) | stored (Open) or ignored (Damaged); a snap different from the open one, other than 0, damages the frame |
| unknown type | ignored | counted, snap-checked |
| record with snap 0 (`LOAD`) | ignored | ignored; never touches the frame, not counted |
| invalid record | ignored | frame becomes Damaged |
| `END` | ignored | closes the frame: *Complete* if its snap equals the frame's, `<count>` equals the number of Curia records of that snap in the frame **including `BEGIN`, `END` and unknown types**, and no rule was broken; otherwise a Damaged event. Then Idle |
| more than **512** records, or more than **128 KiB** buffered, without `END` | n/a | Damaged; nothing more is buffered |
| log truncation | n/a | Damaged event, then Idle |

- The `END` count equalled the number of Curia lines of the frame, `BEGIN` and `END` included, in every complete frame in M3.
- A damaged, incomplete or truncated frame is discarded whole and reported; it is never dropped silently and the app never merges records of two frames.
- **Arrival order is authoritative.** The newest *Complete* frame is the current state. A Damaged event never changes the stored current frame; it sets a flag the UI can show ("last export unreadable") that the next Complete frame clears. `<snap>` is used only to match records within one frame, never to order frames: it continues after loading a save and restarts at 1 in a new game, so it can fall or repeat (M3 T4/T5). A frame whose date is earlier than the previous frame's date (compared as year, month, day; an invalid date gives no hint) is a hint to the app that a save was loaded or a new game started; it is a hint only, since loading a save made on the same day gives equal dates. Frames with `mode` 1 (probe) are frames like any other.
- Two frames are not expected to interleave: in M3 every complete frame arrived as one burst (span 0.000 to 0.016 s). That is an observation, not a proof about script execution; no rapid double click was tested (section 9). Two frames with identical content are two frames; the parser never de-duplicates by text.
- Frame size: the largest frame measured was 81 lines (probe export), about 13 KB in the file with prefixes. The proposed Advisor export is about 52 lines plus 3 per war (section 5). The limit of **512** records is the hard bound.

## 4. Line types

`<n>` numbers items inside a section, starting at 1. Unknown line types are ignored (but counted by `END`).

| Type | Fields (in order) | Notes |
|---|---|---|
| `LOAD` | `mod_version`(token) | `<snap>` is 0. Written when a new game starts. May repeat (two were seen 40 s apart in session 2, cause unknown) and is not written when a save is loaded. Informational; nothing depends on it. |
| `BEGIN` | `mode`(int) `mod_version`(token) `year`(int) `month`(int) `day`(int) `ruler_id`(int) `ruler_name`(**text**) | `mode` 0 = Advisor click, 1 = probe click. `mod_version` lets the app warn about a mod/app mismatch. `ruler_name` is the character's name as the game prints it (for the ruler this carried the title in M0). `mode` and `mod_version` stay the first two fields for the life of `CURIA1`. |
| `FIN` | `gold`(int) `income`(int) | Gold; monthly income (rounded). |
| `HEIR` | `none` **or** `id`(int) `opinion`(int) `has_liege`(0/1) `landed`(0/1) `name`(**text**) | `opinion` is the heir's opinion of the heir's liege (0 when `has_liege` is 0), normally the ruler. |
| `VASSALS` | `total`(int) `listed`(int) | `total` = all direct vassals (the iterator count, which matched the Vassals window in M3); `listed` = the `VASSAL` lines that follow (at most 10, strongest first). |
| `VASSAL` | `rank`(int) `id`(int) `gold`(int) `opinion`(int) `strength`(int) `name`(**text**) | `rank` 1 = strongest by military strength. `opinion` is of the vassal's liege, the ruler for a direct vassal. |
| `WARS` | `total`(int) | Number of wars the ruler is in, including wars joined as an ally. Not capped. |
| `WAR` | `n`(int) `NAME`(enum) `text`(**text**) | Group of three records per war, same `n`. |
| `WAR` | `n`(int) `ATTACKER`/`DEFENDER`(enum) `id`(int) `name`(**text**) | The app compares the ids with `ruler_id` to tell the ruler's side from an ally's. |
| `CLAIMS` | `explicit`(int) `implicit`(int) `listed`(int) | Totals of the ruler's explicit and implicit claims, and the `CLAIM` lines that follow (explicit only, at most 10). See section 6. |
| `CLAIM` | `n`(int) `kind`(`strong`/`weak`) `tier`(int) `title`(**text**) | `strong` = pressed, `weak` = unpressed. `tier` 1 barony to 5 empire. |
| `FACTIONS` | `against`(int) `member`(0/1) `listed`(int) | `against` = factions targeting the ruler; `member` = the ruler belongs to a faction; `listed` = faction groups that follow (at most 5 against, plus the ruler's own). See section 7. |
| `FACTION` | `n`(int) `INFO`(enum) `role`(`against`/`member`) `type`(int) `power`(int) `threshold`(int) `members`(int) | First record of a faction group. `type`: 1 independence, 2 liberty, 3 claimant, 4 populist, 5 peasant, 6 nomadic, 7 other (also any type the mod does not know). |
| `FACTION` | `n`(int) `LEADER`/`CLAIMANT`/`TARGET`(enum) `none` **or** `id`(int) `name`(**text**) | The other three records of the group, in that order, each exactly once. `CLAIMANT` is `special_character`, written only for type 3 (`none` otherwise); the app reads it as a claimant only when `type` is 3. |
| `END` | `count`(int) | Last record. |
| `PROBE` | `name` `value...` | **Not part of the format.** Diagnostic records written only when `mode` is 1. Treated as an unknown type (never as invalid) in every build except test and diagnostic builds. |

The mod's earlier spike output uses the same marker with another layout (`BEGIN` led by a date string, `LOAD` as `mod=curia|version=0.1.0`). It is not `CURIA1`-compatible: a new app reading it reports "unsupported mod format" (section 3), and mod and app are updated together. No release has shipped that layout (see the question on the marker in section 12).

Example frame (artificial names; real frames carry the player's game text):

```
CURIA1|7|BEGIN|0|0.1.0|1066|9|15|1001|Test Ruler One
CURIA1|7|FIN|312|9
CURIA1|7|HEIR|1002|84|1|0|Test Heir Two
CURIA1|7|VASSALS|2|2
CURIA1|7|VASSAL|1|1003|120|55|1523|Test Vassal Three
CURIA1|7|VASSAL|2|1004|40|-12|310|Test Vassal Four
CURIA1|7|WARS|0
CURIA1|7|CLAIMS|1|0|1
CURIA1|7|CLAIM|1|weak|4|Test Realm
CURIA1|7|FACTIONS|1|0|1
CURIA1|7|FACTION|1|INFO|against|3|33|100|2
CURIA1|7|FACTION|1|LEADER|1004|Test Vassal Four
CURIA1|7|FACTION|1|CLAIMANT|1005|Test Claimant Five
CURIA1|7|FACTION|1|TARGET|1001|Test Ruler One
CURIA1|7|END|15
```

## 5. Size

Proposed Advisor export, worst case: `BEGIN`, `FIN`, `HEIR` (1 line), `VASSALS` and 10 `VASSAL` (11), `WARS` and 3 per war (1 + 3w), `CLAIMS` and 10 `CLAIM` (11), `FACTIONS` and 4 per faction for up to 6 factions (25), `END` (1): 52 lines plus 3 per war. The average of about 165 bytes per line in the file, prefixes included (155 to 173 per frame; 64 lines were 10,604 bytes), was measured on the old probe and export frames, not on the proposed lines (`FACTION`, `VASSALS` and `CLAIMS` lines never ran); on that basis roughly 9 KB per click plus about 0.5 KB per war. A typical realm is far less. With wars uncapped (W8), a realm with 154 or more wars exceeds 512 records and is lost whole ("too large"). The 17MB figure for the log is anecdotal and untested (C3); the app's byte budget (`architecture.md` §4) is unchanged.

## 6. Claims

Recommendation (for the owner's decision): **`CLAIM` lines list explicit claims only, each with `strong`/`weak`; implicit claims are given as a count.**

Evidence (session 2):
- In both rulers tested, each with one claim, the explicit claim set equalled the claims the character window lists (one unpressed in one realm, one pressed in the other). The unfiltered iterator and the existing claim counts equal `explicit = all, pressed = all`.
- One ruler's claim was **explicit and unpressed** (flags: explicit-weak only). The heir of that realm had three claims, all implicit and counted as pressed. No ruler with an implicit claim has been observed, and the heir's claims were not compared with the UI.
- Lines are capped at 10; listing explicit claims first keeps the cap for the claims that carry a strong/weak state, and the `implicit` total still tells the advisor that inherited claims exist.

Mod forms:
- Counts: `explicit` and `implicit` from `every_claim` with `explicit = yes` / `explicit = no` and `pressed = all` (ran for ruler and heir: heir 3 implicit, ruler 0).
- Lines: `ordered_claim`, which has so far run only unfiltered or with `explicit = all, pressed = all` (the claim detail probe); `explicit = yes` on `ordered_claim` has **never run**, and the game's own files pass `explicit`/`pressed` only to `any_claim`/`every_claim`. To not depend on it, the filter is also written as a `limit` on the explicit strong and weak claim triggers of the ruler (`has_strong_claim_on`, `has_weak_claim_on`; the game's lists define them as explicit pressed and explicit unpressed), so that the cap of 10 applies to explicit claims. Both forms are **new** (section 8).
- `kind` from the explicit-strong and explicit-weak flags the probe already sets (ran in the probe, section 8); `listed` is `min(explicit, 10)`, computed before the lines are written.

Alternative (not recommended): list all claims with a four-state kind. It costs more lines and an unobserved case for rulers.

## 7. Factions

- The faction name is **not exported**. Even the `NoTooltip` form carries link markup (control bytes, `,`, `:` and `;` inside); that record was the longest line seen in session 2 (section 1). Exported instead: `type`, and the characters as three records with ids and plain names (`GetNameNoTooltip` forms were clean in M3).
- `role`: `against` = the faction targets the ruler; `member` = the faction the ruler has joined (`joined_faction`, a single link in the game's lists, which is why `member` is 0 or 1). The against factions come from `ordered_targeting_faction` with `max = 5`, `check_range_bounds = no` and `order_by = faction_power` (strongest first), so that a cap of 5 drops the weakest. The game's lists document only `any_targeting_faction`, no vanilla file uses `ordered_targeting_faction` and the form has **never run**; fallback: list all against factions and let the app cap (the parser does not rely on the cap, section 3). `FACTIONS.against` always gives the true total.
- `CLAIMANT` answers "who might rebel and for whom": the faction's special character (`special_character`). The link exists in the game's lists and in vanilla script, but its meaning depends on the faction type: the installed faction definitions give peasant, populist and other faction types a special character too, with a "leader" title for peasant and populist factions, while the claimant faction's is the claimant. Hence the mod writes `CLAIMANT` only inside a guard on type 3 (`faction_is_type = claimant_faction` and `exists = special_character`) and writes `none` otherwise, and the app reads it as a claimant only when `type` is 3. **This form has never run**; the pre-freeze check also records which types return a value.
- Left out of `CURIA1` until understood: `discontent` (0 in all three factions seen, so its scale is unknown), `can_press_demands` (1 on a faction whose power was below its threshold, 0 on a claimant faction also below its threshold), `dangerous` (1 on the independence faction, 0 on claimant and peasant), `at_war` (always 0). They stay in `PROBE` lines. Adding them later is a new line type, not a change (section 10).
- Forms: `type` from `faction_is_type`-based codes; `power` and `threshold` from script values (integers); `members` from the member count. For power, two forms were compared: both gave 33 for the claimant faction (window showed 32 percent) and 29 versus 30 for the independence faction (not compared with the UI), so the choice of the script-value form as the single form is arbitrary until the cause of the differences is found; consumers treat the value as approximate (±1). `power` can exceed 100 (170 on a peasant faction, whose threshold was 0).

## 8. Evidence per field

Status: **UI** = ran in the game and matched the game's UI; **ran** = ran, printed a plausible value, not compared; **new** = form has never run (needs the pre-freeze check).

| Field | Status |
|---|---|
| `BEGIN` mode, ruler id | ran |
| `BEGIN` ruler name | UI; the printed ruler name carries the title (M0) |
| `BEGIN` year, month, day | **new** (the date string form ran; the numeric date functions are in the dumps; month and day base unknown) |
| `BEGIN` mod_version, `LOAD` mod_version | **new** (a literal in the localization file; the spike's `LOAD` has another shape) |
| `FIN` gold | UI (no run recorded with a value of 1,000 or more) |
| `FIN` income | ran (rounded: 8 for a HUD value of 7.8) |
| `HEIR` name, opinion, has_liege, landed | ran (opinion is of the heir's liege, not compared with the UI; checked only for a courtier heir in M3) |
| `HEIR` id | ran as a probe |
| `HEIR` none form | **new** |
| `VASSALS` total | UI (the iterator count matched the Vassals window; the probe count form) |
| `VASSALS` listed | **new** (a clamped value computed before the lines; the existing counter is only valid after the loop) |
| `VASSAL` rank, gold, opinion, name | ran; id and strength ran as probes; whether a vassal's printed name carries the title is unverified |
| `WARS` total | **new** (count form) |
| `WAR` NAME | ran for one named historic war only; names of generated claim or CB wars were never exported (markup risk) |
| `WAR` ATTACKER, DEFENDER names | ran; ids **new** |
| `CLAIMS` explicit, implicit | ran (`every_claim` with filters, ruler and heir) |
| `CLAIMS` listed | **new** |
| `CLAIM` kind, tier | ran in the probe (claim detail), 2 rulers with 1 claim each, one unpressed and one pressed |
| `CLAIM` line form (`ordered_claim` with `explicit = yes`, `limit` on strong/weak triggers) | **new** |
| `FACTIONS` against, member | **new** (count forms; the `joined_faction` path ran as a probe) |
| `FACTIONS` listed, cap of 5 (`ordered_targeting_faction`, `max`, `order_by`) | **new** |
| `FACTION INFO` type, members | UI (claimant faction); types 1 and 5 ran |
| `FACTION INFO` power | ran; differs from the window by 1 (window 32, probe 33) |
| `FACTION INFO` threshold | ran (not compared with the UI) |
| `FACTION INFO` role | ran as probe sources (targeting and joined); the `role` enum itself **new** |
| `LEADER`, `TARGET` names | UI; ids **new** (the id form ran for other characters); `none` forms **new** |
| `CLAIMANT` | **new**; which faction types return a value is unknown |

## 9. Pre-freeze check and open assumptions

The freeze is not final until a short in-game check confirms every **new** row of section 8 against the UI, with `errcount` 0. The mod is changed to emit exactly this format first; the app work (watcher, parser, fuzz, fake CK3, fixtures) does not wait for the check. The realms needed:

| Case | Needs | Covers |
|---|---|---|
| A | a claimant faction against the ruler | `CLAIMANT`, ids, `FACTION` group, `role` against |
| B | a faction the ruler has joined (and one the ruler leads) | `member`, joined path |
| C | a peasant or populist faction | which types return a `special_character`; the guard |
| D | a war, ideally two, one generated claim or CB war | `WARS`, `WAR` ids, war-name markup |
| E | more than 10 vassals, and gold and strength of 10,000 or more, a negative opinion and income | `listed` clamp, plain digits (no grouping separator or abbreviation) for the `\|0` forms, whether `-0` occurs, whether vassal names carry the title |
| F | a ruler (or the heir as a stand-in) with implicit claims, and more than 10 claims | the `CLAIM` filter forms, the cap |
| G | more than 5 targeting factions (or an accepted gap) | `ordered_targeting_faction` cap and order |
| H | a heir whose liege is not the ruler (or an accepted gap); the date in the first frames | `HEIR` opinion meaning; month and day base |
| I | two clicks in rapid succession | frames do not interleave |
| J | error-entry lines in `debug.log` after a mod error (on purpose, in a throwaway game) | whether an error entry can quote the marker |

**Mod changes needed** (hidden work behind the format): the `LOAD` literal changes from `mod=curia|version=0.1.0` to one token, and the version string then lives in `descriptor.mod`, the `on_action` file and the `BEGIN` localization, so `tools/ci/lint_mod.py` gets a check that the three agree; every `none` and enum variant needs its own localization key (a `debug_log` prints one key, and the only proven conditional is a boolean `Select_CString`); every `debug_log` site needs a preceding `curia_export_lines` increment (a missed one makes the `END` count wrong and the whole frame is discarded), and the lint checks it; new globals (faction index, the claim-kind flags now needed outside probe mode) need a reading script value and a reset in `curia_export_begin`; the clamped `listed` values; and the old probe lines (`claim detail` has the title in the middle) are migrated or removed, since `PROBE` records are an unknown type.

Open assumptions, none verified: other game languages (the mod ships English text only; whether keys fall back to English, and whether `|0` prints a grouping or decimal separator, is untested: a parser that rejects non-integers makes this fail visibly instead of silently); a very long vassal, title or war name; names containing quotes, braces or newlines (M3 deferred these); how the engine writes a game string that contains a newline.

## 10. Compatibility rules

- The marker `CURIA1|` and every field above stay as they are for the life of `CURIA1`.
- **Additive only:** new data arrives as a **new line type**, or a new sub-kind of `FACTION` or `WAR` (unknown types and sub-kinds are ignored by older apps, counted and snap-checked). A field is never added to, removed from or reordered in an existing type; a changed meaning needs `CURIA2|`. Because the text field is always last, this is what keeps old parsers correct.
- **Enum value sets and int code sets are closed** for the life of `CURIA1`: an unknown enum word is invalid, so a new claim kind, role or faction type is not added inside `CURIA1`. The mod maps types it does not know to 7 (W6f), so a new faction type does not break old apps.
- The app may support more than one marker version at a time.

## 11. Parser and tests (Phase 2)

- `parse_line(bytes) -> optional<Record>` is a pure function (no I/O, no allocation surprises on hostile input); the frame assembler (the state machine of section 3) consumes records; the watcher feeds bytes. These three are separate so the fuzz target and unit tests need no files.
- Tests: fixtures of **bare** `CURIA1|...` records only (plainly artificial names such as "Test Ruler One"), with the engine-style prefix added at test time by the fixture loader and by `tools/fake_ck3` (a prefix is never stored in a tracked file; the pattern scan forbids it). Cases: every line type, damaged frames of each kind, every row of the invalid-record table and the state-machine table, interleaved non-Curia lines, a name containing `|`, markup bytes (the worked sample of section 2), truncation at launch, a falling snapshot number, repeated frames, oversize record, 512-record limit, unterminated tail, invalid UTF-8.
- libFuzzer target on `parse_line` and on the frame assembler, seed corpus from the fixtures; end-to-end CI test: fake CK3 → watcher → parser → mock LLM.

## 12. Questions for the owner

Answer in the same pass as the table of W1 to W12 (accept or defer; each can be added later as a new line type under section 10).

| # | Question | Accept / defer |
|---|---|---|
| Q1 | An oversize record (a very long name) is invalid and damages the frame. Prefer instead that the app truncates the text and keeps the frame? | |
| Q2 | Keep the marker `CURIA1` although the spike layout differs (no release shipped it), or start at `CURIA2`? | |
| Q3 | Wars: uncapped (W8) or capped at 5 with `total` and `listed`? | |
| Q4 | Does `CLAIMANT` also want the title the claimant faction targets (`Faction.GetSpecialTitle` exists in the dumps)? | |
| Q5 | Ruler's own standing: prestige, piety, stress, age, health, traits, tier, government, culture, religion, succession law, and whether the ruler has a liege (a new player is often a vassal). | |
| Q6 | The heir's claims (the only implicit claims seen were the heir's three); the recommended `implicit` count describes the ruler and is nearly always 0. | |
| Q7 | Military picture: the ruler's own strength and levy, war score and casus belli per war. | |
| Q8 | Vassal traits and age, and which vassals are in a faction or plot. | |
| Q9 | Schemes and plots against the ruler, pending decisions and alerts. | |
| Q10 | A game date or speed field, so the "pause the game" hint can react to the frame. | |

Documents changed on approval: `docs/architecture.md` §4 (the sample frame and the sentence that promises "ASCII-safe field escaping defined once in wire-format.md", which becomes "no escaping, text last"), `docs/milestones.md` (M3 criterion 4) and `docs/spikes/m3-results.md` (the verdict row for criterion 4).
