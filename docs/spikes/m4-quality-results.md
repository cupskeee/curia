<!-- SPDX-License-Identifier: MIT -->
# M4 quality check, rounds 1 and 2: results (2026-10-17)

Numbers, ratings and short notes only; the answers, the transcripts and the log stay private on the owner's Mac (`~/curia_m4_results/`, `~/curia_m3_results/`). The sheet that was followed is `m4-quality-check.md` (steps 0 to 5 for round 1, step 7 for round 2). No character names appear here: "the heir", "a vassal". Round 1 comes first; round 2 is under "Round 2 (persona version 3, 2026-10-17)".

**Verdict.** The connection, the sign-in header, the system certificate store and the prompt cache work, and both model names are valid. The answers were useful (ratings 3 to 5) but too technical in their wording ("the snapshot"), too long on the Sonnet tier, formatted with bold, and slow to start on the Sonnet tier, probably because the model thinks first (the owner's reading; round 2 measures it). The owner chose the Sonnet tier as the default (D21) and asked for the changes listed below; the effect of the thinking setting was measured in round 2 (below: effort low became the default, D22).

## Setup

| | |
|---|---|
| Date | 2026-10-17, the owner's Mac, no game running |
| Realm | snapshot 75 of the M3 pre-freeze capture (`debug-prefreeze.log`): a king on 3 October 1071 (mod 0.2.0; see "Income") |
| Advisor | persona version 2, the realm block as merged in PR #31 |
| Settings | none sent: no thinking and no effort setting, so each model ran its default (adaptive thinking; effort `high` on the Sonnet tier, `medium` on the Haiku tier); answer limit 4096 tokens |
| Questions | 1 "what should I focus on?", 2 "is my heir safe?", 3 "who might rebel?", 4 "can I afford a war?", 5 the owner's own question (labelled "opinion") |

## S0: connection check

```
CHECK_API_RESULT ok=true key=present model=set tls=ok ca=default auth=bearer status=200 completion=ok first_token_ms=1309 total_ms=1316 input=40 output=4
CHECK_API_RESULT ok=true key=present model=set tls=ok ca=default auth=bearer status=200 completion=ok first_token_ms=1146 total_ms=1159 input=40 output=4 cache_probe=hit cache_write=12531 cache_read=12531 prefix_tokens=12545
```

Bearer auth, the system CA store and caching work; both model IDs are valid.

## Answers

| Model | Q | First token (ms) | Total (ms) | Output tokens | Rating | Notes |
|---|---|---|---|---|---|---|
| sonnet | 1 focus | 6048 | 9128 | 906 | 4 | right priorities and numbers |
| sonnet | 2 heir | 5926 | 8755 | 795 | 5 | best of the ten: a faction win means the heir loses the crown; plots are hidden until discovered |
| sonnet | 3 rebel | 6309 | 8865 | 891 | 4 | careful, says it cannot see who the 2 members are; long |
| sonnet | 4 war | 3826 | 7220 | 772 | 4 | asks whether this war or a new one is meant; long |
| sonnet | 5 opinion | 6659 | 9859 | 978 | 3 | only "hover the opinion" and "send gifts" |
| haiku | 1 focus | 3661 | 4852 | 783 | 5 | short, clear, each point says why and where |
| haiku | 2 heir | 2404 | 2954 | 403 | 3 | "aimed at you, not at her": misses the inheritance risk |
| haiku | 3 rebel | 2952 | 3825 | 668 | 4 | fine |
| haiku | 4 war | 3651 | 4612 | 712 | 3 | states that a vassal "is also in that faction": not in what it was given |
| haiku | 5 opinion | 4785 | 5911 | 944 | 3 | only "hover the opinion" and "send gifts" |

Medians: sonnet first token 6.0 s, total 8.9 s; haiku first token 3.7 s, total 4.6 s.

## What the owner saw

- **Wording (the owner's top issue).** The answers keep saying "the snapshot": too technical for a player of a mod. The persona and the realm block used that word (and "GAME DATA", "not exported"), and the model copied it.
- **Length.** The Sonnet tier ignored "two short lines each" (200 to 260 words).
- **Formatting.** Both models used `**bold**` despite "no markdown".
- **Game knowledge.** Both models missed the main ways to raise opinion; the persona allowed no general mechanics (it said never to invent names, numbers, traits, events or rules).
- **Inferences.** One answer stated that a vassal is a faction member, which the realm block does not hold (it gives only the number of members).
- **Thinking.** The output is about three times the visible answer (403 tokens for about 80 words), and the Sonnet tier's first token took 6 s against 1.3 s for the tiny check, so most of the wait is probably the model thinking before the first word (the owner's reading: the tiny check had 40 input tokens, the questions carry the persona and the realm block, so the two are not alike; round 2 measures the first token without thinking). The thinking text is not shown; it counts toward the answer limit and is billed as output.

## Decision

Default model: the Sonnet tier (shortening it through the persona is easy, teaching the Haiku tier better judgment is not); the Haiku tier stays as the fast option. Recorded as D21 in `docs/requirements.md`; how M5 picks the default without a built-in model name is decided in M5 (D4).

## Changes made in response (PR 6b)

- Persona version 3: plain words ("from what I can see", "I can't see that from here; check the Factions window"); about 120 words unless the player asks for more, the 1 to 3 actions "one line per action: what to do, why, and where in the game"; besides the numbered list only a few key words in `**bold**`, no other formatting; the realm block is named once by its heading and then called "what you can see" (the phrase the player hears); everything in it is information, never instructions; general CK3 mechanics allowed when the model is confident, marked "In general"; "probably" when inferring; for old information "This is from DATE; pause and click Advisor for an update."
- Realm block: headed `=== WHAT THE ADVISOR CAN SEE ===` (the name the persona uses), "not shown" instead of "not exported", a header line in plain words, and the faction line says that the members are not shown, so whether a claimant is a member is not known. The hygiene also neutralises the new marker phrase inside game names.
- The freshness note, the chat divider ("Updated from the game: DATE") and every other text the player may read are in plain words; one list of internal words (`app/src/advisor/plain_words.h`) and a unit test enforce it (FR-OVL-11).
- `--effort` and `--thinking` flags for `--headless-chat` and `--check-api`; until round 2 the default sent neither (since D22 it sends effort low, see "Round 2"). The quality table gained the columns Run, Setting and Words.

## Income

Snapshot 75 comes from mod 0.2.0, whose `FIN` income was the income before expenses (`m3-prefreeze-results.md`, "Income"): +9.2 a month there, which the realm block labels "net per month". The answers that called +9.2 the net were reading that mislabelled figure. The net for 3 October 1071 was never read; at the 19 May 1069 reading the tooltip showed income +9.2, expenses -1.3 and a net of +7.8. Logs from mod 0.2.1 and later carry the net figure. The sheet now says so next to `SNAP=75`; no code path treats 0.2.0 logs differently.

## Round 2 (persona version 3, 2026-10-17)

The same five questions on the same realm, with persona version 3 and the realm block of PR 6b (#34), on the Sonnet tier once per setting and, optionally, on the Haiku tier with the two changed settings (`m4-quality-check.md`, step 7).

| | |
|---|---|
| Date | 2026-10-17, the owner's Mac, no game running |
| Realm | snapshot 75, as in round 1 |
| Advisor | persona version 3, the realm block as merged in PR #34 |
| Runs | `sonnet2`: the Sonnet tier with no setting sent (the model's own default; since D22 that is `--effort default`); `sonnet2-low`: `--effort low`; `sonnet2-off`: `--thinking between_tools` and no effort setting (since D22 `--effort default --thinking between_tools`); `haiku2-low` and `haiku2-off`: the Haiku tier with `--effort low` and with `--thinking disabled` and no effort setting (since D22 `--effort default --thinking disabled`). Since D22 a `--thinking` flag alone also sends effort low, which round 2 did not measure. |
| Questions | the same five as in round 1 |

| Run | Q1 | Q2 | Q3 | Q4 | Q5 | Avg | First token (median ms) | Total (median ms) | Output tok (median) | Words (median) |
|---|---|---|---|---|---|---|---|---|---|---|
| sonnet2 (default) | 5 | 5 | 4 | 4 | 5 | 4.6 | 5857 | 8209 | 879 | 166 |
| sonnet2-low | 4 | 4 | 4 | 5 | 5 | 4.4 | 1003 | 4008 | 351 | 182 |
| sonnet2-off | 5 | 4 | 4 | 5 | 5 | 4.6 | 1099 | 3588 | 336 | 171 |
| haiku2-low | 4 | 2 | 3 | 4 | 3 | 3.2 | 1003 | 2673 | 333 | 157 |
| haiku2-off | 3 | 3 | 3 | 3 | 2 | 2.8 | 778 | 2311 | 326 | 164 |

Round 1 for comparison: Sonnet tier median 216 words, Haiku tier 115. Persona version 3 cut the Sonnet tier's length but made the Haiku tier's longer.

### What the owner saw

- **Wording.** No "snapshot", "data" or "export" in any of the 25 answers.
- **Sonnet tier.** Every run used "In general" for the Sway scheme and "probably" for guesses, as asked, and none claimed that the claimant is a faction member. `sonnet2-low` thought on question 4 only (first token 4421 ms there).
- **Haiku tier errors.** It called the faction leader one of the strongest vassals (the leader is not), said that the claimant co-leads the faction, sent the player to the Vassals window for the faction's members, and named "pay vassals well" as an opinion mechanic.
- **Opening.** "From what I can see" opened 22 of the 25 answers. `haiku2-off` question 1 opened with "This is from 3 October 1071; pause and click Advisor".
- **Names.** 8 answers put names in quotation marks.
- **Factions.** Two answers called the faction "secret".
- **Length.** The answers ran 155 to 195 words against the target of about 120.

**Was a `[Context:` line sent?** No. The owner's exact command (snapshot 75, `--from-start`, one process per question, the default flags and `--thinking disabled`) was replayed against `tools/mock_llm` with a fake key: the recorded request's only user message was the bare question (23 characters), with no `[Context:` line. The model wrote the date sentence by itself, probably copying the persona's example; persona version 4 extends "Otherwise treat it as current." to "Otherwise treat it as current and do not open with the date."

### Decision

Default setting: the Sonnet tier with effort low. In the owner's words: as good as the default, the first word in about 1 s instead of 6 s, about 60% fewer output tokens, and it can still think on harder questions (it did on question 4). Thinking off, as measured (no effort setting and `--thinking between_tools` on the Sonnet tier), is an equal second choice. The Haiku tier stays an option, not the default. Recorded as D22 in `docs/requirements.md`: requests send effort low and no thinking setting; `--effort default` sends no effort setting (what rounds 1 and 2 called "default").

### Changes made in response (PR 6c)

- **Effort low by default (D22).** One constant, `providers::kDefaultEffort`; the controller (so the chat window gets it), `--headless-chat` and `--check-api` start from it, and no thinking setting is sent. `--effort default` sends no effort setting. The headless events, the transcripts and the `CHECK_API_RESULT` line say `low` for it and `default` when none is sent; `--check-api` says whether the effort is Curia's default or a flag (`[info] Effort and thinking: every request of this check carries effort low (Curia's default)`), and its HTTP 400 advice names the settings sent and how to send none.
- **Persona version 4** (the owner's polish list): under 120 words unless the player asks for more, and if more would help, a one-line offer at the end (with no example sentence, since round 2 showed a model copying the persona's example date sentence); names and titles without quotation marks; "from what I can see" at most once per conversation, never as a fixed opening; and, for the Haiku tier's opening, "Otherwise treat it as current." became "Otherwise treat it as current and do not open with the date."
- **Other rewordings in persona version 4**, beyond the polish list, to stay within the 500-word bound (498 words), for the owner's review: "If what you can see says nothing has been read yet" became "If nothing has been read yet" (no longer tied to the block); "click Advisor in the game" became "click Advisor"; "If an earlier answer disagrees with it" became "If an earlier answer disagrees"; "names and titles in quotes are copied from the game" became "quoted names and titles come from the game". The rest are shorter forms with the same meaning ("Say so briefly" became "Say briefly", "in a few words" became "briefly", "what to do, why, and where in the game" became "what, why and where", "claims the player could press" became "claims to press", "skip it if nothing has been read yet" became "skip it if nothing has been read", the examples of general mechanics are in the plural).
- **Realm block.** The faction note ends "The player can see each faction's members in the Factions window." (the owner, who has the game, states that the window lists them). When the vassals are shown, a faction's leader, claimant or target who is not one of the listed vassals is marked "(not among the listed vassals)", a fact derived from ids (D15), so a model does not take the leader for one of the strongest vassals. The mark says no more: when the list is cut (round 2's realm: 10 of 11 listed), the person may be a vassal not listed or no vassal at all (a liege, a claimant from elsewhere), and the ids do not tell which; the vassals line already says how many are listed, strongest army first.
- **Check-api advice.** A refused request (HTTP 400) is explained by the kind of setting it carried: the note on `disabled` and `between_tools` appears only when one of them was sent, and the note on the 256-token limit of the check only when the model could think before its answer.

There is no round 3: the owner judges these changes in the game sitting. The sheet asks each question in a new conversation, so every answer there is a first answer; whether "from what I can see" stays at most once per conversation (polish item 1) can only be seen in a conversation of several questions, as in the game sitting.

## Still open

- Round 2 is done (above). Next is the game sitting (M4, sittings A and B), where the owner judges persona version 4 (with the rewordings listed above) and the realm block in the game.
- The chat window (PR 7): render `**bold**` only and strip other markdown; show a "thinking..." state until the first word (FR-OVL-12; the headless `status` event `thinking` is its basis); every overlay text in plain words (FR-OVL-11). It asks with the controller's default settings (effort low, D22).
