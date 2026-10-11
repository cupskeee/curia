# Curia wire format `CURIA1` (approved with amendments, not yet frozen)

Status: **approved by the owner at the wire-format checkpoint of 2026-10-11, with the amendments recorded in the decision table below; not frozen.** The format becomes the frozen `CURIA1` specification only after the mod has been changed to emit it and the pre-freeze in-game check (section 9) has passed; until then a row marked **new** in section 8 can still change. The mod's output on `main` is still the spike layout until the mod change merges, so this file describes the *approved* output.

What it is: the contract between the mod (`mod/curia/`, writes) and the app (`app/`, reads) through CK3's `debug.log`. Evidence is in `docs/spikes/m3-results.md` (session 1, the PR #11 re-check and the session of 2026-10-11, cited as "session 2" in this file). The engine facts are in `docs/research-notes.md` §2.14 and §2.16.

## Owner decisions (checkpoint of 2026-10-11)

Every item below was accepted by the owner on 2026-10-11; the amended ones say how. The reasons are in the section named.

| # | Decision (as accepted) | Section |
|---|---|---|
| W1 | Every line has at most one free-text field and it is last; no escaping. A pipe or markup in a game string cannot move another field | 2 |
| W2a | The title text is dropped from `BEGIN` and `VASSAL`. **Amended:** already verified by the owner, so it is not part of the pre-freeze check: the printed vassal names of session 1 and the faction leader and target names of session 2 carried the title (as the ruler's name did in M0) | 4, 8 |
| W2b | Ids are added (heir, vassals, war sides, faction characters) so characters can be matched across records | 4 |
| W3a | Section header lines with totals (`VASSALS`, `WARS`, `CLAIMS`, `FACTIONS`) replace the old `none` lines, so a skipped section is told apart from an empty one | 3, 4 |
| W3b | `VASSALS`, `CLAIMS` and `FACTIONS` carry a `listed` field (the number of item records that follow); `WARS` has only `total`, because wars are not capped | 4 |
| W3c | Group and count rules are normative and checked by the parser (table in section 3) | 3 |
| W4 | The date is three integers plus a `mod_version` token in `BEGIN`; the localized date string is dropped | 4 |
| W5 | Claims: `CLAIM` lines list **explicit** claims only, each `strong` or `weak`; implicit claims are a count. **Amended:** the app's prompt uses the UI's words (pressed and unpressed) although the enum is `strong`/`weak` | 6 |
| W6a | Factions carry no name | 7 |
| W6b | Faction data: type, power, threshold, member count, and the `LEADER`, `CLAIMANT`, `TITLE` and `TARGET` records | 4, 7 |
| W6c | `CLAIMANT` is `special_character`, written by the mod only for type 3, and read by the app as a claimant only for type 3. **Added with Q4:** a `TITLE` record carries the title the claimant faction targets (section 7) | 4, 7 |
| W6d | At most 5 factions against the ruler (strongest by power, through an ordered iterator that has never run) plus the ruler's own | 7 |
| W6e | `discontent`, `can_press_demands`, `dangerous` and `at_war` stay out of `CURIA1` until their meaning is established. **Amended:** `discontent` can come back as a new `FACTION` sub-kind once a faction above its threshold has been observed (it was 0 in all three factions seen) | 7 |
| W6f | Faction type codes 1 to 7 (section 4), with 7 absorbing any type the mod does not know | 4 |
| W7 | One form per field: numbers come from script values as integers (power, income, strength), consumers treat them as approximate. **Amended:** `FIN` `income` is the monthly income in **tenths** of a gold piece (7.8 becomes 78); gold stays whole. A new player's count earns 1 to 5 gold a month, where whole-gold rounding is off by up to 0.5 gold (up to 50 percent of a 1-gold income, about 10 percent of a 5-gold income), and under W9 `FIN` cannot change later inside `CURIA1`. The form is part of the pre-freeze check | 2, 4, 8 |
| W8 | Wars are not capped (Q3); `WARS` gives the total, and frames above the 512-record limit are lost. `ATTACKER` and `DEFENDER` carry ids so the app can tell which side the ruler is on, or that the ruler is only an ally | 3, 4 |
| W9 | Additive-only compatibility inside `CURIA1`: new data is a new line type or sub-kind, fields are never added to a type, enum value sets are closed | 2, 10 |
| W10 | Arrival order decides which frame is current; the snapshot number only matches records inside a frame; a date that goes back is a hint of a reload; the frame lifecycle is the state machine of section 3 | 3 |
| W11 | Committed fixtures hold bare `CURIA1\|...` records with plainly artificial names; the engine-style prefix is added at test time | 11 |
| W12 | The mod is changed to emit this format first (list in section 9), then a short in-game check of about 30 minutes covers the **must-test** rows of section 9; the cases the owner accepted as gaps are listed there with what a failure looks like. Only then is the format declared frozen | 9 |

Design rule added by the owner at the same checkpoint (recorded in `requirements.md`, D15): **Curia exports only what the player could see in the game's own UI** (no hidden schemes, secrets or undiscovered plots). It applies to every field here and decides Q8 and Q9 when they come back. An "omniscient" mode could be a later opt-in.

How this maps to the owner's four requests of 2026-10-11 (the answers are the accepted decisions above): (1) markup or pipes cannot inject a field: W1 and W6a (sections 2 and 7); (2) implicit versus explicit claims: W5 (section 6); (3) one form per field: W7 and W6e (sections 2 and 7); (4) a claimant field: W6c (section 7).

Not wire-format, listed because it was proposed earlier: a "pause the game first" hint in the overlay (the game keeps running while the player types) is an app behaviour for M4, not part of this file. The questions asked at the checkpoint and the owner's answers are in section 12.

## 1. Transport

- The mod writes one `debug_log` call per record. CK3 appends each as one line of `debug.log` (UTF-8, `\n` terminated).
- **Only a line terminated by `\n` is a record.** An unterminated tail is held back by the watcher and never parsed (a torn read would otherwise give a valid record with a wrong number); after a truncation of the file it is discarded. Exactly one `\r` immediately before the `\n` is removed. The line-length limit is **4,096 bytes**, measured on the physical line before the `\r` is removed and without the `\n`: a longer line keeps its first 4,096 bytes (the head); the rest, up to the next `\n`, is dropped, and the line is marked *overlong*. A line whose head holds no `CURIA1|` is not a record.
- The engine puts its own prefix in front of every such line (observed 119 to 124 bytes in session 1, 119 to 131 bytes in session 2: time, level, source, script path, effect name). The prefix is not part of the format and its shape is not relied on. The **record** is the text from the first occurrence of `CURIA1|` to the end of the line. A marker at offset 0 (no engine prefix at all) is not a record: the engine always writes a prefix, and this stops the tail of a game string that contained a newline from forging a record.
- Lines without `CURIA1|` are ignored. Non-Curia lines may sit between two Curia lines of one frame (the engine's multi-line error entries did in M3); they are ignored and not counted. Whether an error entry can quote the marker is not known (section 9).
- The log is truncated in place at game launch (same inode, size back to 0). That is the watcher's concern (`architecture.md` §5), not the format's: after a truncation any open frame is abandoned (section 3).
- Records are small: the longest Curia line seen was 199 bytes in session 1 and 321 bytes in session 2 (a faction name line of the probe export; the prefix is included in both). **Oversize records (owner decision Q1, 2026-10-11).** One long name must not discard a whole snapshot. A record is measured from the first `C` of the marker to the end of the line (the end of the head of an overlong line), without the `\r` and `\n`. A record is *oversize* if it is longer than **1,024 bytes** or comes from an overlong line (its tail was dropped, so it counts as oversize even when the head is shorter than 1,024 bytes):
  - a record of a known type **with a text field**, whose fixed fields and the `|` before the text lie completely within the first 1,024 bytes, is **valid with a shortened text**. The record is cut to its first 1,024 bytes, then up to 3 trailing bytes are dropped if they are the start of a UTF-8 sequence whose continuation bytes are not all present (the cut never leaves half a character; other invalid bytes are kept and repaired later by hygiene step 1). The frame is flagged *text shortened*: a note, not damage, and the frame stays Complete;
  - any other oversize record of a known type is **invalid**;
  - an **unknown type** (and a known type with an unknown sub-kind) is not inspected beyond its header (section 2), so it is exempt from this rule;
  - an oversize record counts once in the `END` count, and its bytes count as the cut record (the byte cap of section 3).
- Threat model: frames are not authenticated. Anyone who can write to `debug.log` can forge records. A game string can at most change its own text field; it can never alter another field (section 2). The one residual risk is a newline inside a game string: the text after it becomes a new physical line, which is a record only if it contains the marker after at least one byte (the marker at offset 0 is refused), and any such forged record still has to pass the snap, count and structure checks of section 3. Nothing else defends against it.

## 2. Record syntax

```
CURIA1|<snap>|<TYPE>|<field>|<field>|...
```

- Fields are separated by `|`. There is **no escaping**: the mod cannot escape game text.
- **Order of evaluation** for every physical line (a parser reports one of three outcomes: *not a Curia line*, *invalid record*, or a *record*, section 11):
  1. No `CURIA1|` in the line (head), or the marker at offset 0: not a Curia line.
  2. The header must parse: `<snap>` an int (grammar below), `<TYPE>` matching `[A-Z][A-Z0-9_]*`. Otherwise (`CURIA1|`, `CURIA1||BEGIN`, `CURIA1|abc|FOO`, a type outside the pattern) the record is invalid.
  3. Snap 0 is valid only on `LOAD`. `LOAD` with a snap of 1 or more, and any other type with snap 0 (`BEGIN`, `END`, ...), is invalid. A snap-0 record, valid or invalid, never touches a frame (section 3); an invalid one is only reported to the app's log.
  4. A type that matches the pattern but is not in the table of section 4 is an **unknown type**; so is a known `FACTION` or `WAR` whose sub-kind is not in the table. For these only the header is validated: the snap, the type, and for `FACTION` and `WAR` an int `n` and a sub-kind word matching `[A-Z][A-Z0-9_]*` (a missing, empty or non-matching sub-kind, or a non-int `n`, is invalid). The rest of the line is not inspected and is exempt from the byte-class rule and the oversize rule; the record is ignored but **counted** and snap-checked. `PROBE` records are an unknown type and so are never invalid.
  5. A known type is checked against the field grammar below and the oversize rule (section 1).
- Field kinds:
  - **int**: `0`, or an optional `-` and then `[1-9][0-9]{0,9}`. Leading zeros, a lone `-`, a `+` sign, grouping separators, decimals, exponents and non-ASCII digits are invalid. The value must fit int32 (-2,147,483,648 to 2,147,483,647). `-0` is valid **only in a signed field** and is read as 0 (the game's rounding of a small negative number may print it; section 9). Snap equality is numeric on this canonical form.
  - **enum**: one of the listed ASCII words, case-sensitive. The value sets are **closed** for the life of `CURIA1` (section 10); an unknown word is invalid. The sub-kind word of `FACTION` and `WAR` is not an enum value in this sense: an unknown sub-kind makes an unknown type (step 4).
  - **token**: `[A-Za-z0-9._+-]{1,32}` (a version string).
  - **text**: any bytes after the fixed fields (UTF-8 expected).
  An empty field in a non-text position is invalid. Bytes below `0x20` or `0x7F` and above (anything not ASCII) are only allowed in a text field of a known type; there is no trimming outside text.
- **Ranges** (an out-of-range value is invalid):

  | Fields | Range |
  |---|---|
  | `gold`, `income`, `opinion` (heir and vassal) | signed: any int32 |
  | `snap` | 0 to 2,147,483,647 (0 only on `LOAD`, step 3) |
  | `rank`, `n`, ids, `strength`, `power`, `threshold`, `members`, `total`, `listed`, `explicit`, `implicit`, `against`, the `END` count | 0 to 2,147,483,647 (`rank` and `n` start at 1) |
  | `mode` | 0 or 1 |
  | `has_liege`, `landed`, `member` | 0 or 1 |
  | `tier` | 1 to 5 |
  | `type` | 1 to 7 |
  | `year` | 0 to 9999 |
  | `month`, `day` | 1 to 12 and 1 to 31 (changed only by a documented decision if the pre-freeze check shows another base); a calendar-impossible date such as 31 in a 30-day month is accepted |

- **Every line type has at most one text field and it is always the last field.** A text-bearing record has exactly its fixed fields, then a mandatory `|`, then the text, which may be empty (a game string can be empty, or emptied by hygiene) and may contain `|`. A record without text has exactly its fields. A game string containing `|` can therefore never move or change another field.
- The line types that allow `none` are `HEIR` and the `FACTION` sub-kinds `LEADER`, `CLAIMANT`, `TITLE` and `TARGET`. Each has two forms, the `none` form and the full form; the `none` form has no bytes after the word `none` (a trailing `|` or text is invalid).
- A record that breaks a rule above is **invalid**: it is dropped and the open frame is damaged (section 3). An oversize text field is not a broken rule (section 1).

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
| `7\|FACTION\|1\|TITLE\|none\|` | bytes after `none` |
| `7\|FACTION\|1` | `FACTION` without a sub-kind |
| `0\|BEGIN\|0\|0.1.0\|1066\|9\|15\|1\|x` | snap 0 is valid only on `LOAD` |
| `7\|VASSALS\|-0\|0` | `-0` in a non-negative field |

Valid and notable: `7|HEIR|1002|84|1|0|` (empty name), `7|HEIR|none`, `7|FIN|-0|-0` (`gold` and `income` are signed), `7|FACTION|2|DISCONTENT|42` (an unknown sub-kind: ignored, counted), an unknown-type record of any length or content, and a `VASSAL` whose name pushes the record over 1,024 bytes (valid, name shortened, frame flagged *text shortened*).

- **Text hygiene** (applied by the app to every text field, as defence in depth; the format does not depend on it), in this order: (1) repair invalid UTF-8 (U+FFFD); (2) delete the two bytes `0x15 0x21`; (3) after a byte `0x15`, the *tag word* is the maximal run of non-space bytes that follows it; if the tag word is exactly `L` or `L;`, or begins with `ONCLICK:` or `TOOLTIP:` (only the tags seen are known), delete the `0x15`, the tag word and one following U+0020 (up to the end of the text if no space follows); (4) delete every remaining `0x15` and every other code point below U+0020, U+007F, U+0080 to U+009F, U+2028, U+2029, U+202A to U+202E and U+2066 to U+2069; (5) collapse runs of U+0020 and trim U+0020. Worked sample (`^U` is the byte `0x15`, ids and names artificial): `Install ^UONCLICK:CHARACTER,1 ^UTOOLTIP:CHARACTER,1 ^UL Test Ruler^U!^U!^U! on the ^UONCLICK:TITLE,2 ^UTOOLTIP:LANDED_TITLE,2 ^UL; Test Realm^U!^U!^U! Throne` becomes `Install Test Ruler on the Test Realm Throne`. Hygiene is applied by the frame assembler when it copies a text field into the frame, never by the line parser. This sample is a fixture. Faction names are no longer exported (section 7); character and title names in the `NoTooltip` forms were clean in M3, so hygiene is expected to change nothing for them.
- Numbers never carry decimals, grouping separators or units. Values the game computes as fractions are rounded by the mod to an integer; consumers treat them as approximate (power was seen to differ from the UI by 1, section 7; income in tenths is rounded and has not been compared with the UI, section 8).
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
| `FACTIONS` | `listed` equals the number of `FACTION` groups; `n` runs 1..`listed`; each group is `INFO`, `LEADER`, `CLAIMANT`, `TITLE`, `TARGET` adjacent, in that order, exactly once each; the number of groups with `role` `member` equals `member`; the number with `role` `against` is <= `against` |

**State machine of the frame assembler.** States: *Idle*, *Open(snap)*, *Damaged(snap)*. A *frame record* is any Curia record with snap 1 or more that reaches the assembler: every known type, `BEGIN` and `END` included, and every unknown type. Snap-0 records are never frame records. The assembler emits at most **one** Damaged event per frame: when the frame is first damaged, or, if it was never damaged, when it is abandoned (a new valid `BEGIN`, an invalid `BEGIN`, or a truncation of the log). The event carries the snap and a reason: *incomplete*, *invalid record*, *snap mismatch*, *count mismatch*, *order*, *too large* or *truncated log*.

| Event | Idle | Open(snap) | Damaged(snap) |
|---|---|---|---|
| valid `BEGIN` (snap >= 1) | open a frame (Open); a shortened `ruler_name` sets *text shortened* on it | the open frame is abandoned (Damaged event, reason *incomplete*), a new frame opens | no event (already reported); a new frame opens |
| invalid `BEGIN` with an int snap of 1 or more | report "unsupported mod format" (the snap and the raw text of the first three fields after the type, at most 64 bytes, control bytes shown as `?`); stay Idle | Damaged event for the open frame (*incomplete*), then the report; Idle | the report; Idle |
| any other known type, snap >= 1 | ignored (the watcher may attach mid-frame) | stored; a snap different from the frame's damages it (*snap mismatch*); a shortened text sets *text shortened* | ignored |
| unknown type, snap >= 1 | ignored | counted; a snap different from the frame's damages it | ignored |
| invalid record whose snap is 1 or more, or whose snap does not parse | ignored (reported to the app's log only) | Damaged (*invalid record*) | ignored |
| any record with snap 0 | ignored (an invalid one is reported to the app's log) | ignored; never touches the frame, not counted | ignored |
| `END` (snap >= 1) | ignored | closes the frame: *Complete* if its snap equals the frame's, `<count>` equals the number of frame records **including `BEGIN`, `END` and unknown types**, the section rules and checked counts above hold, and no other rule was broken (a shortened text is not a broken rule; the Complete event carries *text shortened*); otherwise a Damaged event (*snap mismatch*, *count mismatch* or *order*). Then Idle | Idle, no event |
| the **513th** frame record (even if it is `END`), or frame-record bytes above **128 KiB** (the sum of the records' lengths from marker to end, after the cut of section 1) | n/a | Damaged (*too large*); nothing more is stored | n/a |
| log truncation | n/a | Damaged event (*truncated log*), then Idle | Idle, no event |

- The `END` count equalled the number of Curia lines of the frame, `BEGIN` and `END` included, in every complete frame in M3.
- A damaged, incomplete or truncated frame is discarded whole and reported with one Damaged event; it is never dropped silently and the app never merges records of two frames.
- **Arrival order is authoritative.** The newest *Complete* frame is the current state. A Damaged event never changes the stored current frame; it sets a flag the UI can show ("last export unreadable") that the next Complete frame clears. `<snap>` is used only to match records within one frame, never to order frames: it continues after loading a save and restarts at 1 in a new game, so it can fall or repeat (M3 T4/T5). A frame whose date is earlier than the date of the previous *Complete* frame (compared as year, month, day; calendar-impossible dates are compared as they are) is a hint to the app that a save was loaded or a new game started; it is a hint only, since loading a save made on the same day gives equal dates. Frames with `mode` 1 (probe) are frames like any other.
- Two frames are not expected to interleave: in M3 every complete frame arrived as one burst (span 0.000 to 0.016 s). That is an observation, not a proof about script execution; no rapid double click was tested (section 9). Two frames with identical content are two frames; the parser never de-duplicates by text.
- Frame size: the largest frame measured was 81 lines (probe export), about 13 KB in the file with prefixes. The Advisor export is at most 58 lines plus 3 per war (section 5). The limit of **512** records is the hard bound.

## 4. Line types

`<n>` numbers items inside a section, starting at 1. Unknown line types are ignored (but counted by `END`).

| Type | Fields (in order) | Notes |
|---|---|---|
| `LOAD` | `mod_version`(token) | `<snap>` is 0. Written when a new game starts. May repeat (two were seen 40 s apart in session 2, cause unknown) and is not written when a save is loaded. Informational; nothing depends on it. |
| `BEGIN` | `mode`(int) `mod_version`(token) `year`(int) `month`(int) `day`(int) `ruler_id`(int) `ruler_name`(**text**) | `mode` 0 = Advisor click, 1 = probe click. `mod_version` lets the app warn about a mod/app mismatch. `ruler_name` is the character's name as the game prints it (for the ruler this carried the title in M0). `mode` and `mod_version` stay the first two fields for the life of `CURIA1`. |
| `FIN` | `gold`(int) `income`(int) | `gold` is whole gold. `income` is the monthly income in **tenths** of a gold piece, rounded to an integer (78 means 7.8; may be negative, `-0` is read as 0). Owner decision W7. The form is new (section 8). |
| `HEIR` | `none` **or** `id`(int) `opinion`(int) `has_liege`(0/1) `landed`(0/1) `name`(**text**) | `opinion` is the heir's opinion of the heir's liege (0 when `has_liege` is 0), normally the ruler. |
| `VASSALS` | `total`(int) `listed`(int) | `total` = all direct vassals (the iterator count, which matched the Vassals window in M3); `listed` = the `VASSAL` lines that follow (at most 10, strongest first). |
| `VASSAL` | `rank`(int) `id`(int) `gold`(int) `opinion`(int) `strength`(int) `name`(**text**) | `rank` 1 = strongest by military strength. `opinion` is of the vassal's liege, the ruler for a direct vassal. |
| `WARS` | `total`(int) | Number of wars the ruler is in, including wars joined as an ally. Not capped. |
| `WAR` | `n`(int) `NAME`(enum) `text`(**text**) | Group of three records per war, same `n`. |
| `WAR` | `n`(int) `ATTACKER`/`DEFENDER`(enum) `id`(int) `name`(**text**) | The app compares the ids with `ruler_id` to tell the ruler's side from an ally's. |
| `CLAIMS` | `explicit`(int) `implicit`(int) `listed`(int) | Totals of the ruler's explicit and implicit claims, and the `CLAIM` lines that follow (explicit only, at most 10). See section 6. |
| `CLAIM` | `n`(int) `kind`(`strong`/`weak`) `tier`(int) `title`(**text**) | `strong` = pressed, `weak` = unpressed. `tier` 1 barony to 5 empire. Highest tier first (ties in the game's iterator order), at most 10 explicit claims. |
| `FACTIONS` | `against`(int) `member`(0/1) `listed`(int) | `against` = factions targeting the ruler; `member` = the ruler belongs to a faction; `listed` = faction groups that follow (at most 5 against, plus the ruler's own, if the capped iterator of section 7 works; otherwise all against). See section 7. |
| `FACTION` | `n`(int) `INFO`(enum) `role`(`against`/`member`) `type`(int) `power`(int) `threshold`(int) `members`(int) | First record of a faction group. `type`: 1 independence, 2 liberty, 3 claimant, 4 populist, 5 peasant, 6 nomadic, 7 other (also any type the mod does not know). |
| `FACTION` | `n`(int) `LEADER`/`CLAIMANT`/`TARGET`(enum) `none` **or** `id`(int) `name`(**text**) | Records of the group after `INFO`, in the order `LEADER`, `CLAIMANT`, (`TITLE`, next row), `TARGET`, each exactly once, all with the same `n` as `INFO`. `CLAIMANT` is `special_character`, written only for type 3 (`none` otherwise); the app reads it as a claimant only when `type` is 3, and a value in a group of another type is valid and ignored (the same holds for `TITLE`). |
| `FACTION` | `n`(int) `TITLE`(enum) `none` **or** `tier`(int) `title`(**text**) | The title a claimant faction wants (`special_title`), between `CLAIMANT` and `TARGET`; `none` unless `type` is 3 and the faction has a special title. Added with Q4 (accepted if it fits case A of the check; otherwise it is removed before the freeze). `tier` 1 barony to 5 empire. |
| `END` | `count`(int) | Last record. |
| `PROBE` | `name` `value...` | **Not part of the format.** Diagnostic records written only when `mode` is 1. Treated as an unknown type (never as invalid) in every build except test and diagnostic builds. |

The mod's earlier spike output uses the same marker with another layout (`BEGIN` led by a date string, `LOAD` as `mod=curia|version=0.1.0`). It is not `CURIA1`-compatible: a new app reading it reports "unsupported mod format" (section 3), and mod and app are updated together. No release has shipped that layout (see the question on the marker in section 12).

Example frame (artificial names; real frames carry the player's game text):

```
CURIA1|7|BEGIN|0|0.1.0|1066|9|15|1001|Test Ruler One
CURIA1|7|FIN|312|78
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
CURIA1|7|FACTION|1|TITLE|4|Test Realm
CURIA1|7|FACTION|1|TARGET|1001|Test Ruler One
CURIA1|7|END|16
```

## 5. Size

Advisor export, worst case: `BEGIN`, `FIN`, `HEIR` (1 line), `VASSALS` and 10 `VASSAL` (11), `WARS` and 3 per war (1 + 3w), `CLAIMS` and 10 `CLAIM` (11), `FACTIONS` and 5 per faction for up to 6 factions (31), `END` (1): 58 lines plus 3 per war. The average of about 165 bytes per line in the file, prefixes included (155 to 173 per frame; 64 lines were 10,604 bytes), was measured on the old probe and export frames, not on the new lines (`FACTION`, `VASSALS` and `CLAIMS` lines never ran); on that basis roughly 10 KB per click plus about 0.5 KB per war. A typical realm is far less. With wars uncapped (W8), a frame holds at most 151 wars if every other section is at its maximum (169 if all other sections are empty); one with more than 512 records is lost whole ("too large"). If the capped faction iterator of section 7 fails in case A and the cap of 5 is dropped from the format, the worst case grows by 5 lines per extra faction against the ruler. The 17MB figure for the log is anecdotal and untested (C3); the app's byte budget (`architecture.md` §4) is unchanged.

## 6. Claims

Decision W5 (accepted): **`CLAIM` lines list explicit claims only, each with `strong`/`weak`; implicit claims are given as a count.**

Evidence (session 2):
- In both rulers tested, each with one claim, the explicit claim set equalled the claims the character window lists (one unpressed in one realm, one pressed in the other). The unfiltered iterator and the existing claim counts equal `explicit = all, pressed = all`.
- One ruler's claim was **explicit and unpressed** (flags: explicit-weak only). The heir of that realm had three claims, all implicit and counted as pressed. No ruler with an implicit claim has been observed, and the heir's claims were not compared with the UI.
- Lines are capped at 10, highest tier first; the cap applies to explicit claims, which carry the strong/weak state, and the `implicit` total still tells the advisor that inherited claims exist.

Mod forms:
- Counts: `explicit` and `implicit` from `every_claim` with `explicit = yes` / `explicit = no` and `pressed = all` (ran for ruler and heir: heir 3 implicit, ruler 0).
- Lines: `ordered_claim`, which has so far run only unfiltered or with `explicit = all, pressed = all` (the claim detail probe); `explicit = yes` on `ordered_claim` has **never run**, and the game's own files pass `explicit`/`pressed` only to `any_claim`/`every_claim`. To not depend on it, the filter is also written as a `limit` on the explicit strong and weak claim triggers of the ruler (`has_strong_claim_on`, `has_weak_claim_on`; the game's lists define them as explicit pressed and explicit unpressed), so that the cap of 10 applies to explicit claims. Both forms are **new** (section 8).
- `kind` from the explicit-strong and explicit-weak flags the probe already sets (ran in the probe, section 8); `listed` is `min(explicit, 10)`, computed before the lines are written.

Wording (owner decision W5): the enum stays `strong`/`weak` (the game's trigger names), but the app's prompt and any text shown to the player use the UI's words: `strong` = a **pressed** claim, `weak` = an **unpressed** claim.

Alternative (not recommended, not chosen): list all claims with a four-state kind. It costs more lines and an unobserved case for rulers.

## 7. Factions

- The faction name is **not exported**. Even the `NoTooltip` form carries link markup (control bytes, `,`, `:` and `;` inside); that record was the longest line seen in session 2 (section 1). Exported instead: `type`, and the characters as three records with ids and plain names (`GetNameNoTooltip` forms were clean in M3).
- `role`: `against` = the faction targets the ruler; `member` = the faction the ruler has joined (`joined_faction`, a single link in the game's lists, which is why `member` is 0 or 1). The against factions come from `ordered_targeting_faction` with `max = 5`, `check_range_bounds = no` and `order_by = faction_power` (strongest first), so that a cap of 5 drops the weakest. The game's lists document only `any_targeting_faction`, no vanilla file uses `ordered_targeting_faction` and the form has **never run**; fallback, if the ordered form fails in case A: the cap of 5 is dropped from the format before the freeze, `listed` becomes `against` plus `member`, section 5 is recomputed without a faction limit, and the app keeps the 5 strongest by `power` for its prompt (the parser does not rely on the cap or the order, section 3). `FACTIONS.against` always gives the true total.
- `CLAIMANT` answers "who might rebel and for whom": the faction's special character (`special_character`). The link exists in the game's lists and in vanilla script, but its meaning depends on the faction type: the installed faction definitions give peasant, populist and other faction types a special character too, with a "leader" title for peasant and populist factions, while the claimant faction's is the claimant. Hence the mod writes `CLAIMANT` only inside a guard on type 3 (`faction_is_type = claimant_faction` and `exists = special_character`) and writes `none` otherwise, and the app reads it as a claimant only when `type` is 3. **This form has never run**; case A of the pre-freeze check covers the claimant faction, and which other types return a value is an accepted gap (case C).
- `TITLE` (Q4): the title a claimant faction wants, from the faction's `special_title` link (guarded by `has_special_title` and by type 3), written with the proven title-scope name form and the tier script value. **Never run.** It is tested in case A of the check; if it does not work there, it is removed from the format before the freeze (the owner's condition for accepting Q4).
- Visibility (design rule D15): the factions listed are the ones the Factions window shows; case A compares them.
- Left out of `CURIA1` until understood: `discontent` (0 in all three factions seen, so its scale is unknown), `can_press_demands` (1 on a faction whose power was below its threshold, 0 on a claimant faction also below its threshold), `dangerous` (1 on the independence faction, 0 on claimant and peasant), `at_war` (always 0). They stay in `PROBE` lines. Adding them later is a new line type or a new `FACTION` sub-kind, not a change (section 10); the owner expects `discontent` to return as a sub-kind once a faction above its threshold has been observed.
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
| `FIN` income (tenths) | **new** (the whole-gold form ran: 8 for a HUD value of 7.8; the tenths form and its sign have never run) |
| `HEIR` name, opinion, has_liege, landed | ran (opinion is of the heir's liege, not compared with the UI; checked only for a courtier heir in M3) |
| `HEIR` id | ran as a probe |
| `HEIR` none form | **new** |
| `VASSALS` total | UI (the iterator count matched the Vassals window; the probe count form) |
| `VASSALS` listed | **new** (a clamped value computed before the lines; the existing counter is only valid after the loop) |
| `VASSAL` rank, gold, opinion, name | ran; id and strength ran as probes; the printed name carries the title (owner-verified, session 1); whether the Vassals window shows a vassal's gold and strength (design rule D15) is checked in case E (income part) of section 9 |
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
| `FACTION INFO` threshold | ran (not compared with the UI; whether the Factions window shows it, design rule D15, is checked in case A) |
| `FACTION INFO` role | ran as probe sources (targeting and joined); the `role` enum itself **new** |
| `LEADER`, `TARGET` names | UI; the printed names carry the title (owner-verified, session 2); ids **new** (the id form ran for other characters); `none` forms **new** |
| `CLAIMANT` | **new**; which faction types return a value is unknown |
| `TITLE` | **new** (`special_title`, `has_special_title`; the title-scope name and tier forms ran for claims) |

## 9. Pre-freeze check and open assumptions

The freeze is not final until a short in-game check (about 30 minutes, owner decision of 2026-10-11) confirms the **must-test** rows below against the UI, with `errcount` 0. The mod is changed to emit exactly this format first; the app work (watcher, parser, fuzz, fake CK3, fixtures) does not wait for the check. The checklist the owner follows is `docs/spikes/m3-prefreeze-checklist.md`.

Every click also exercises, in whatever realm it is made, the new rows that need no special setup: the numeric date, `mod_version`, the `WARS`, `CLAIMS` and `FACTIONS` counts, the clamped `listed` values and the `END` count. The parser's count and order rules (section 3) turn a wrong form into a visible damaged frame, never into silent wrong data.

**Must-test** (the realms needed):

| Case | Needs | Covers |
|---|---|---|
| A | a claimant faction against the ruler (the throwaway king of session 2 also has one explicit claim, so the `CLAIM` line form runs in the same realm) | `FACTION` group, `role` `against`, ids, `CLAIMANT`, `TITLE` (Q4, kept only if it works), the `CLAIM` line form, `ordered_targeting_faction` (if it fails, the cap of 5 is dropped, section 7), the factions listed against the Factions window; design rule D15 for `threshold`: whether the Factions window shows it (a field that fails D15 is removed before the freeze) |
| B | a faction the ruler has joined or leads (an independence faction created as a vassal, as in session 2) | `member`, the joined path, `role` `member`; the `none` forms of `CLAIMANT` and `TITLE` (neither applies to an independence faction) |
| D | a war, ideally two, one of them a generated claim or CB war | `WARS`, `WAR` ids (the ruler's side or an ally), war-name markup |
| I | two clicks in rapid succession | frames do not interleave |
| E (income part) | a realm with at least one vassal; ideally also a negative income | `FIN` income in tenths and its sign (`-0`), plain digits; **at least one negative integer printed and read back** among `income`, `gold` and `opinion` (a vassal or heir with negative opinion is an easy way): the printed form of the minus sign has never been observed, and a non-ASCII or marked-up minus would make every such frame invalid; D15: whether the Vassals window shows a vassal's gold and strength |
| H (date part) | the first frames of any game | numeric year, month and day against the HUD date; month and day base |

**Accepted gaps** (the owner accepts them unless they turn out to be easy to set up; a gap is listed in the results as untested, not as passed). If an edge case needs setup (10,000 or more gold, many claims or vassals), a separate throwaway game in debug mode with console commands is fine, as long as debug mode does not change what is measured (number formatting, list caps and the like). What a failure looks like:

| Case | Gap | If the form is wrong |
|---|---|---|
| C | a peasant or populist faction (which types return a `special_character`; the `none` form of `LEADER`, which a leaderless faction prints) | the guard keeps `CLAIMANT` at `none` outside type 3, and the app reads it as a claimant only for type 3, so a wrong guess is ignored; a wrong `LEADER` `none` key makes the record invalid and the frame is discarded visibly, every time that faction exists |
| E (rest) | more than 10 vassals (the `listed` clamp), gold and strength of 10,000 or more (plain digits, no grouping separator or abbreviation) | `listed` is checked against the items (section 3), and a grouping separator makes an int invalid: the frame is damaged and reported, so a rich realm would fail visibly, every time. The highest-risk gap; the console route above is the cheap way to close it |
| F | a ruler with implicit claims, more than 10 claims | `CLAIMS` `listed` and the item count are checked; a wrong filter shows as a damaged frame |
| G | more than 5 factions against the ruler | `FACTIONS` `against` is the true total whatever is listed; the parser does not rely on the cap or the order |
| H (rest) | an heir whose liege is not the ruler | `opinion` would describe the heir's liege, not the ruler; `has_liege` is there for it |
| J | error-entry lines in `debug.log` after a mod error (on purpose, in a throwaway game) | a quoted marker adds a record, so the `END` count no longer matches and the frame is discarded visibly |
| `HEIR` `none`, `TARGET` `none` | a ruler with no heir, a faction with no target | the `none` key prints other text, the record is invalid and the frame is discarded visibly |

**Mod changes needed** (hidden work behind the format): the `LOAD` literal changes from `mod=curia|version=0.1.0` to one token, and the version string then lives in `descriptor.mod`, the `on_action` file and the `BEGIN` localization, so `tools/ci/lint_mod.py` gets a check that the three agree; every `none` and enum variant needs its own localization key (a `debug_log` prints one key, and the only proven conditional is a boolean `Select_CString`); every `debug_log` site needs a preceding `curia_export_lines` increment (a missed one makes the `END` count wrong and the whole frame is discarded), and the lint checks it; new globals (faction index, the claim-kind flags now needed outside probe mode) need a reading script value and a reset in `curia_export_begin`; the clamped `listed` values; the `income` script value in tenths (`monthly_character_income` times 10); the `TITLE` record (`special_title`, guarded by type 3 and `has_special_title`, with its own `none` key); the `CLAIMANT` guard; and the old probe lines (`claim detail` has the title in the middle) are migrated or removed, since `PROBE` records are an unknown type.

Open assumptions, none verified: other game languages (the mod ships English text only; whether keys fall back to English, and whether `|0` prints a grouping or decimal separator, is untested: a parser that rejects non-integers makes this fail visibly instead of silently); a very long vassal, title or war name; names containing quotes, braces or newlines (M3 deferred these); how the engine writes a game string that contains a newline.

## 10. Compatibility rules

- The marker `CURIA1|` and every field above stay as they are for the life of `CURIA1`.
- **Additive only:** new data arrives as a **new line type**, or a new sub-kind of `FACTION` or `WAR` (unknown types and sub-kinds are ignored by older apps, counted and snap-checked). A field is never added to, removed from or reordered in an existing type; a changed meaning needs `CURIA2|`. Because the text field is always last, this is what keeps old parsers correct.
- **Enum value sets and int code sets are closed** for the life of `CURIA1`: an unknown enum word is invalid, so a new claim kind, role or faction type is not added inside `CURIA1`. The mod maps types it does not know to 7 (W6f), so a new faction type does not break old apps.
- The app may support more than one marker version at a time.

## 11. Parser and tests (Phase 2)

- Layers, each a pure function or small class with no I/O, so the fuzz target and the unit tests need no files: (1) the **line splitter** takes appended bytes and returns complete physical lines (it holds an unterminated tail, drops the `\r`, applies the 4,096-byte head and marks overlong lines); (2) `parse_line(line, overlong)` takes one physical line and returns one of *not a Curia line*, *invalid record* (with the snap and the leading fields when they parsed) or a *record* (snap, type, sub-kind, typed fields, raw text after the oversize cut, and the *text shortened* flag); it applies the marker search and the offset-0 rule, the order of evaluation and the grammar of section 2 and the oversize rule of section 1, but not hygiene; (3) the **frame assembler** (the state machine of section 3) consumes the results, checks snap, counts and order, applies hygiene to text fields, and emits Complete and Damaged events and the unsupported-format report; (4) the watcher feeds bytes to the splitter.
- Tests: fixtures of **bare** `CURIA1|...` records only (plainly artificial names such as "Test Ruler One"), with the engine-style prefix added at test time by the fixture loader and by `tools/fake_ck3` (a prefix is never stored in a tracked file; the pattern scan forbids it). Cases: every line type, damaged frames of each kind, every row of the invalid-record table and the state-machine table, interleaved non-Curia lines, a name containing `|`, markup bytes (the worked sample of section 2), truncation at launch, a falling snapshot number, repeated frames, an oversize text field (shortened at a UTF-8 boundary, frame kept and flagged), an oversize record without a text field (invalid), an overlong physical line, 512-record limit, unterminated tail, invalid UTF-8.
- libFuzzer target on `parse_line` and on the frame assembler, seed corpus from the fixtures; end-to-end CI test: fake CK3 → watcher → parser → mock LLM.

## 12. Questions for the owner (answered 2026-10-11)

| # | Question | Answer |
|---|---|---|
| Q1 | An oversize record (a very long name) is invalid and damages the frame. Prefer instead that the app truncates the text and keeps the frame? | **Accepted.** Truncate an oversize text field, keep the frame and flag it; the fixed fields must still parse (sections 1 and 3) |
| Q2 | Keep the marker `CURIA1` although the spike layout differs, or start at `CURIA2`? | **Keep `CURIA1`** |
| Q3 | Wars: uncapped (W8) or capped at 5? | **Uncapped**, as W8 |
| Q4 | Does `CLAIMANT` also want the title the claimant faction targets? | **Accepted if it fits into case A of the check, otherwise deferred:** the `TITLE` record (sections 4 and 7) |
| Q5 | Ruler's own standing: prestige, piety, stress, age, health, traits, tier, government, culture, religion, succession law, and whether the ruler has a liege | **Deferred**; each returns as a new line type (W9). Priority for M6a: succession law, whether the ruler has a liege, stress, age and health, prestige and piety, then war score and the ruler's own strength |
| Q6 | The heir's claims | **Deferred** |
| Q7 | Military picture: the ruler's own strength and levy, war score and casus belli per war | **Deferred** (war score and the ruler's own strength are in the M6a priority list) |
| Q8 | Vassal traits and age, and which vassals are in a faction or plot | **Deferred**; under design rule D15 only what the UI shows |
| Q9 | Schemes and plots against the ruler, pending decisions and alerts | **Deferred**; under design rule D15 only what the UI shows (no hidden schemes, secrets or undiscovered plots) |
| Q10 | A game date or speed field | **Deferred** (the date is in `BEGIN`) |

Documents changed on the freeze: `docs/architecture.md` §4 (the sample frame and the sentence that promises "ASCII-safe field escaping defined once in wire-format.md", which becomes "no escaping, text last"), `docs/milestones.md` (M3 criterion 4) and `docs/spikes/m3-results.md` (the verdict row for criterion 4).
