<!-- SPDX-License-Identifier: MIT -->
# M4 quality check, round 1: results (2026-10-17)

Numbers, ratings and short notes only; the answers, the transcripts and the log stay private on the owner's Mac (`~/curia_m4_results/`, `~/curia_m3_results/`). The sheet that was followed is `m4-quality-check.md` (steps 0 to 5). No character names appear here: "the heir", "a vassal".

**Verdict.** The connection, the sign-in header, the system certificate store and the prompt cache work, and both model names are valid. The answers were useful (ratings 3 to 5) but too technical in their wording ("the snapshot"), too long on the Sonnet tier, formatted with bold, and slow to start on the Sonnet tier, probably because the model thinks first (the owner's reading; round 2 measures it). The owner chose the Sonnet tier as the default (D21) and asked for the changes listed below; the effect of the thinking setting is measured in round 2.

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
- `--effort` and `--thinking` flags for `--headless-chat` and `--check-api`; the default still sends neither (D22). The quality table gained the columns Run, Setting and Words.

## Income

Snapshot 75 comes from mod 0.2.0, whose `FIN` income was the income before expenses (`m3-prefreeze-results.md`, "Income"): +9.2 a month there, which the realm block labels "net per month". The answers that called +9.2 the net were reading that mislabelled figure. The net for 3 October 1071 was never read; at the 19 May 1069 reading the tooltip showed income +9.2, expenses -1.3 and a net of +7.8. Logs from mod 0.2.1 and later carry the net figure. The sheet now says so next to `SNAP=75`; no code path treats 0.2.0 logs differently.

## Still open

- Round 2 (`m4-quality-check.md`, step 7): the same questions on the Sonnet tier with the default settings, with `--effort low` and with `--thinking between_tools` (optionally the Haiku tier with `--effort low` and `--thinking disabled`), with the revised persona. Then the owner picks the default setting (D22), set in a small follow-up change.
- The chat window (PR 7): render `**bold**` only and strip other markdown; show a "thinking..." state until the first word (FR-OVL-12; the headless `status` event `thinking` is its basis); every overlay text in plain words (FR-OVL-11).
