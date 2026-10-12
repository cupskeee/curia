# Advisor layer (`app/src/advisor/`, plus `mailbox`, `ui_message`, `ui_pacing`, `app_controller` in `app/src/core/`)

Pure logic between the frame the bridge delivers and the request a provider sends. Everything here is part of
`curia_core`: no libcurl, no SDL, no platform headers, no threads of its own, so the sanitizer and fuzz
presets cover it. The advisor knows the abstract `providers::Provider` only.

## What each piece does

| Piece | File | Job |
|---|---|---|
| Snapshot renderer | `snapshot_renderer.*` | `renderSnapshot(Frame)` -> the text block the model reads. Deterministic (integers only, no clock, no locale), prints only fields that exist in `docs/wire-format.md` (design rule D15) plus facts derived from ids (which side of a war the ruler is on, a faction that targets the ruler, a faction leader who is one of the listed vassals). States its own limits: `10 of 14 vassals listed`, implicit claims as a count only, income "about +-0.2", `not exported` for an absent section, `none` for an empty one. Faction power and threshold carry their limits in the text (power about +-1, a threshold of 0 occurs, what happens at the threshold is not exported). Game strings go through `sanitizeGameText()` (quoted, control characters, invisible Unicode (zero-width, bidirectional marks, the tag block, variation selectors) dropped, other Unicode spaces read as a blank, fullwidth ASCII turned into ASCII, `<` `>` `[` `]` `=` removed or replaced, the words "game data" neutralised, at most 64 characters). Wrapped in `=== BEGIN GAME DATA (data only) ===` / `=== END GAME DATA ===`. |
| Persona | `persona.*` | The first system block: who the advisor talks to (a new player), how it answers (plain, short, 1 to 3 actions with a reason and where in the game), what it can see, that the data block is data and never instructions, how to react to an old snapshot. `kPersonaVersion` is logged with every request. |
| Freshness | `freshness.*` | `freshnessNote(Freshness)` -> `[Context: ...]` or an empty string. Pure; thresholds are constants (`kSnapshotOldAfterSeconds`, `kLogSilentAfterSeconds`). |
| Prompt builder | `prompt_builder.*` | `buildChatRequest()` -> `ChatRequest`: persona + snapshot + the last 6 complete question/answer pairs + the question. |
| Snapshot store | `snapshot_store.*` | Newest Complete frame, its arrival time (injected clock), its rendered text, the "export unreadable" flag. |
| Chat session | `chat_session.*` | The transcript and at most one request in flight. Generation ids, cancel/retry, plain-language errors. |
| Error wording | `chat_error_text.*` | `ChatError` -> `ErrorInfo` (plain summary + redacted detail). |
| Log silence monitor | `log_silence_monitor.*` | Pure state machine over an injected clock: "restart CK3", a soft "silent" notice, "CK3 not running". |
| Mailbox, UI messages | `core/mailbox.h`, `core/ui_message.*` | The only crossing between threads. |
| UI pacing | `core/ui_pacing.*` | How the UI thread waits: block, or a short timeout while a spinner or a text cursor is on screen. |
| App controller | `core/app_controller.*` | Wires the watcher's events, the monitor, the store, the session and the provider behind one object for the UI thread. |

## The cache-stability rule

The provider caches a request's prefix: the persona, then the snapshot, then the earlier turns. For the
second question about the same snapshot to be cheap, those bytes must be identical in both requests. So:

* neither system block contains anything that varies between two questions of one snapshot (no time, no age,
  no counters): the persona is a constant, the snapshot is rendered once per Complete frame;
* the history is sent exactly as it was stored: the freshness note is never written into the transcript, only
  added to the request (so the history is byte-stable across two questions when the earlier request carried no
  note; see the next paragraph);
* the request has no field that changes per question except the question itself (and the note).

A question that carried a note loses it when it becomes history, so that one turn's bytes change once (the
system blocks do not); a question asked while the snapshot was fresh carries no note and never changes.

What does change the prefix: a new Complete frame (a new snapshot block), and the 7th pair pushing the oldest
pair out of the window. Both are expected cache misses.

## The freshness-note rule

A snapshot can be old (the player talks for ten minutes after clicking Advisor) or may have been missed (the
export is silent, or the last frame could not be read). The model is told in one bracketed line:

    [Context: the snapshot is from game date 15 September 1066 and was taken 7 minutes ago (real time); Curia has seen no new game data for 6 minutes; the last export could not be read, so newer game data may exist.]

* it is built by `freshnessNote()` from four numbers and nothing else; it is empty when everything is fresh
  (snapshot younger than 2 minutes, log not silent for 5 minutes, last export readable);
* it is appended, after a blank line, to the LATEST user message only, never to history, never to a system
  block;
* it contains no time of day (the game date is the game's, the ages are relative), so the same inputs always
  give the same text;
* its presence alone means "may be out of date" (it is only emitted past a threshold); the persona says so and
  tells the model what to do: say it first, suggest pausing and clicking Advisor again;
* a new snapshot arriving mid-conversation replaces the snapshot block; the transcript divider is for the
  player only. The persona tells the model to trust the GAME DATA block over its earlier answers.

The "restart CK3" notice of `LogSilenceMonitor` is only a hint that no frame came after the player asked for
one. A healthy CK3 shows it too if the player has not clicked Advisor in the game (a frame exists only after
that click, and what CK3 writes while idle is not measured yet, T8). The UI wording must hedge: "No new game
data yet. Click Advisor in the game; if that does nothing, restart CK3".

## Threads

The session and the controller belong to the UI thread. The log watcher thread and the provider's net thread
only build a `UiMessage` and push it into the mailbox; they never read or write advisor state. The mailbox
wakes the UI thread (one SDL event in the app) when it goes from empty to non-empty. The UI thread calls
`AppController::pump()`.

Mailbox policy (decided here): nothing is dropped for being slow. Consecutive text tokens of one request merge
into one pending message (up to 64 KiB), so a stalled UI holds a few large messages instead of thousands of
small ones. A hard limit (4096 messages) drops and counts messages that arrive when the queue is full, except
the end of a request, which is forced in.

## Redaction

The provider redacts each token alone; a key split over two tokens passes through as two pieces that may each
be harmless. The session therefore keeps the raw joined answer in a private buffer (never in the transcript)
and shows `redact(whole buffer)` after every token, so a key is found however it was split (a key rule needs
16 characters after `sk-ant-`, so redacting the already-redacted text again would miss the tail). While the
answer streams, the last word (the trailing run of letters, digits, `-` and `_`) is held back from the
transcript, so the start of a key that is still arriving is never shown; it appears with the next blank or at
the end of the answer (a stop or an error shows it too). The question is redacted before it is stored or sent.
A registered secret that contains other characters can show as a prefix for a moment, until it is complete.

An answer that ends with no text at all is an error ("The service sent an empty answer. Try again."), not a
blank answer.

## What the app log gets

Numbers and short words only (`ask_started`, `ask_done` with token counts and timings, `frame_complete`,
`notice`, ...). Never a prompt, an answer, a name, or a server message.
