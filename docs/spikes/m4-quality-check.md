# M4 quality check (owner, macOS, no game, about 40 minutes, about $1; round 2 about 30 minutes)

Purpose: before the chat window is built, see what the advisor actually says to a real question about
a real saved realm, judge whether the answers are good enough for a new player, and measure how fast
two models are. Nothing here needs Crusader Kings III to be running: the program reads an old log
file (the M3 pre-freeze capture) and answers questions typed on the command line.

You type nothing into the game and the program writes nothing into the game folders. It does talk to
the real service, which costs money (see "Cost"). The key is typed only into the Terminal window you
run it from.

**Rules for this sheet**
- Never paste the key anywhere: not into a chat message, not into a file, not into this sheet. The
  steps below use a prompt that does not show what you type and does not keep it in the shell history.
- The program never prints the key. If you ever see something that looks like a key in a file or on the
  screen, stop and tell me.
- What you send back is numbers and ratings. The answers stay on your computer unless you decide to
  send an excerpt. They are in **two** places, and neither is to be sent: the text files in
  `~/curia_m4_results/transcripts/`, and the technical files in `~/curia_m4_results/runs/` (every
  answer is inside them too). Step 0.3 makes both folders private to your user.

## 0. Before you start

1. Open **one** Terminal window. Use it for everything below (the key lives only in this window).
2. Build the program from the merged `main`:

   ```sh
   cd ~/PycharmProjects/curia
   git switch main
   git pull --ff-only
   export VCPKG_ROOT=~/vcpkg
   cmake --preset macos-arm64
   cmake --build --preset macos-arm64 --target curia
   ```

   The last line ends with `Linking CXX executable app/curia` (a few minutes the first time).
3. Name the program and the log, and make the result folders (private to your user):

   ```sh
   CURIA=~/PycharmProjects/curia/build/macos-arm64/app/curia
   LOG=~/curia_m3_results/debug-prefreeze.log
   SNAP=75
   FLAGS=()
   umask 077
   mkdir -p ~/curia_m4_results/runs
   chmod 700 ~/curia_m4_results ~/curia_m4_results/runs
   ls -l "$CURIA" "$LOG"
   ```

   `SNAP=75` chooses which saved moment of the realm the questions are about (see step 2).
   `FLAGS=()` means "no extra settings"; round 2 (step 7) changes it. Since D22 (2026-10-17)
   `FLAGS=()` runs Curia's own settings, effort low and no thinking setting; the default runs of
   rounds 1 and 2 sent no setting at all, which is now `FLAGS=(--effort default)`.
   Both files (the program and the log) must be listed; if either is missing, stop and tell me which.

   **About the money figure of snapshot 75.** This log was written by mod 0.2.0, whose income figure
   is the income *before* expenses: +9.2 a month for snapshot 75. The program labels it "net per
   month", so an answer that calls +9.2 a month the net is reading a mislabelled figure, not making a
   mistake of its own; rate the reasoning, not that number. The net for that date (3 October 1071)
   was never read; at the 19 May 1069 reading the tooltip showed income +9.2, expenses -1.3 and a net
   of +7.8. Logs from mod 0.2.1 and later carry the net figure.
4. Type the key. This prompt shows nothing while you type (that is intended); press Enter when done:

   ```sh
   read -s "CURIA_ANTHROPIC_API_KEY?API key (nothing is shown): "
   export CURIA_ANTHROPIC_API_KEY
   echo
   ```

5. Set the first model and switch on the transcripts (the answers are then also saved as text files in
   `~/curia_m4_results/transcripts/`, which is outside the repository):

   ```sh
   export CURIA_MODEL=claude-sonnet-5-5
   export CURIA_LOG_CONTENT=1
   ```

## 1. S0: connection check (about 5 minutes, a few cents)

This checks the key, the model name, the secure connection and the way the key is sent, with one tiny
question.

```sh
"$CURIA" --check-api
```

**What you should see**: a first line `Sending one tiny request (usually a few seconds)...`, then five
lines that start with `[ok]` (since D22 with an `[info]` line after the second, naming the effort
the check sends), an empty line and the `CHECK_API_RESULT` line:

```
Sending one tiny request (usually a few seconds)...
[ok] API key: present (never shown)
[ok] Model: claude-sonnet-5-5
[info] Effort and thinking: every request of this check carries effort low (Curia's default)
[ok] Connection and certificates: connected in 120 ms, certificates checked with the system default certificate store
[ok] Sign-in header (bearer): accepted (HTTP 200)
[ok] Tiny answer: first token after 900 ms, finished after 1200 ms; tokens in 30, out 5, stop reason end_turn

CHECK_API_RESULT ok=true key=present model=set effort=low thinking=default tls=ok ca=default auth=bearer status=200 completion=ok ...
```

(The numbers differ.) A line that starts with `[FAILED]` is explained by lines that start with `->`
(they can sit under the line after it, "Tiny answer"); "Troubleshooting" at the end has the same. Then
the cache check, which sends two long identical questions to see whether the service remembers the
start of the prompt (about 3 cents):

```sh
"$CURIA" --check-api --cache-probe
```

It prints the same lines, a second line at the top (`Cache probe: ...`), and one more line at the end
that starts with `[info] Prompt cache probe:` and says either

```
[info] Prompt cache probe: cache hit: the first request wrote 8310 tokens to the cache and the second read 8310 of them back
       -> first: input=15 cache_write=8310 cache_read=0; second: input=15 cache_write=0 cache_read=8310
```

or `no cache hit: prefix below the model minimum or caching not applied`, followed by the numbers. Both
are results, not errors (`[info]` is not a failure).

Optional, only if the first check said the sign-in header was rejected, or if I ask for it:

```sh
"$CURIA" --check-api --auth x-api-key
```

**Write down:** the `CHECK_API_RESULT ...` line of each run (it holds no secret).

If the first check says `model was not found`: **stop here** and send me the exact model name you
typed (it is not secret). Do not guess another one.

## 2. The quality run with the first model (about 10 minutes)

The program reads the log (the M3 pre-freeze capture, which holds six saved moments of one game, each a
complete snapshot of the realm as the game exported it) and answers each question as a fresh chat.
**Which moment**: the program's own default is the *last* one in the file, which is a fresh start
(snapshot 1: a duke in 1066 with 156 gold and 7 vassals; little to say about). That is why this sheet
uses `SNAP=75` (set in step 0.3): a king in 1071 with 928 gold, 11 vassals of whom several dislike the king,
a faction against the king and a war. If you want to see the other one, set `SNAP=1` and repeat the run
with another label; write the snapshot number next to your ratings (the table has a column for it).

Copy this block once. It only defines two commands and runs nothing:

```sh
ask_one() {
  : "${LABEL:?set LABEL first, for example LABEL=sonnet}"
  if (( ! ${+FLAGS} )); then echo "set FLAGS first, for example FLAGS=() for the default settings"; return 1; fi
  echo "question $1 ($LABEL, ${FLAGS[*]:-default settings}): asking (10 to 60 seconds is normal, nothing is shown until it ends) ..."
  printf 'ask %s\n' "$2" | "$CURIA" --headless-chat --log "$LOG" --from-start --frame "$SNAP" "${FLAGS[@]}" > ~/curia_m4_results/runs/$LABEL-q$1.jsonl
  echo "question $1: exit code $?"
}
run_questions() {
  ask_one 1 "what should I focus on?"
  ask_one 2 "is my heir safe?"
  ask_one 3 "who might rebel?"
  ask_one 4 "can I afford a war?"
}
```

Now name this run and ask questions 1 to 4:

```sh
LABEL=sonnet
run_questions
```

You should see four lines `exit code 0`. Check which moment it used:

```sh
grep -h -m1 '"event":"ready"' ~/curia_m4_results/runs/$LABEL-q1.jsonl
```

It shows `"snapshot":75` and `"date":"3 October 1071"` (and the model name).

Question 5 is yours: anything a new player might ask about this realm. Put it between the single
quotes (**no apostrophe `'`, no double quote, no exclamation mark** in it) and run both lines:

```sh
Q5='write your own question here'
ask_one 5 "$Q5"
```

**Reading the answers.** Open the folder of transcripts and read the newest files. Each file starts
with the model, the snapshot number and the numbers, and then holds the question and the answer, so you
can tell which is which; they are named by the time they were made (UTC), oldest first:

```sh
open ~/curia_m4_results/transcripts
```

Rate each answer from 1 to 5 as a **new player who finds the game overwhelming**:

| Rating | Meaning |
|---|---|
| 5 | clear, short, I know what to do next, nothing in it is wrong |
| 4 | useful, a little long or a little vague |
| 3 | partly useful; I would have to look things up |
| 2 | confusing, too long, or says something I think is wrong |
| 1 | useless or invents things that are not in my realm |

Things to look for: Does it quote the real numbers (gold, the heir's name, vassals' opinions)? Does it
give at most three things to do, each with a reason? Does it use the game's own words? Does it say so
when the snapshot does not cover something, instead of inventing? Write a few words of notes for each
(for example "good, but did not mention the faction").

## 3. The same questions with the second model (about 10 minutes)

Use the model name exactly as given to you. **Do not copy the block of step 2 again**: the two commands
are already defined in this window, and a new label keeps the first model's files safe.

```sh
export CURIA_MODEL=claude-haiku-5-5
LABEL=haiku
run_questions
ask_one 5 "$Q5"
```

If the service answers **model was not found**, stop and send me the exact name you typed.

(If you opened a new Terminal window in between, the commands, `$Q5`, `$CURIA`, `$LOG`, `$SNAP` and the
key are gone: do step 0 again, and the first block of step 2 once.)

Read and rate these answers the same way. The first question of each model is a little slower
(the service stores the start of the prompt); that is expected, mention it if the difference is large.

## 4. The table

```sh
python3 ~/PycharmProjects/curia/tools/m4/quality_table.py ~/curia_m4_results/runs
```

This prints a table with one row per question and run: the run's label (`sonnet`, `haiku`), the model,
the setting (`default` when none was sent, `effort low` for Curia's own settings since D22), the
snapshot number, time to the first token, total time, tokens, the number of words in the answer, the
cache numbers and how the answer ended, plus empty columns for your rating and notes, and then the
median times and words per run label and setting. The rows are in the order of the file names
(`haiku-...` comes before `sonnet-...`); each row names its run and question, so the order does not
matter. Copy it into a text note and fill in the two last
columns (the table below is the shape of what you send back):

| Run | Question | First token (ms) | Total (ms) | Rating 1-5 | Notes |
|---|---|---|---|---|---|
| sonnet | what should I focus on? | | | | |
| sonnet | is my heir safe? | | | | |
| sonnet | who might rebel? | | | | |
| sonnet | can I afford a war? | | | | |
| sonnet | (your own question) | | | | |
| haiku | what should I focus on? | | | | |
| haiku | is my heir safe? | | | | |
| haiku | who might rebel? | | | | |
| haiku | can I afford a war? | | | | |
| haiku | (your own question) | | | | |

## 5. What to send back

Send me, in one message, **only**:

1. The `CHECK_API_RESULT ...` line of each check (step 1).
2. The table of step 4 with your ratings and short notes (no answer text needed).
3. Two sentences: which model you would pick as the default and why, and the one thing that bothered
   you most in the answers.

Do not send transcripts. If you want me to look at one answer, copy only the sentences you mean, and
remove names you do not want to share.

## 6. Clean up

```sh
unset CURIA_ANTHROPIC_API_KEY CURIA_MODEL CURIA_LOG_CONTENT Q5 FLAGS
unset -f ask_one run_questions
grep -l "sk-ant-" ~/curia_m4_results/runs/* ~/curia_m4_results/transcripts/* 2>/dev/null || echo "no key found in the result files"
```

The last line must say `no key found in the result files`. If it lists a file, do not open or share
it; tell me. Closing the Terminal window also forgets the key. The files in `~/curia_m4_results/`
stay on your computer; they hold game text, which is why they are never inside the repository.

## 7. Round 2: after the persona revision (about 30 minutes, about 30 cents)

**Done (2026-10-17).** Round 2 has been run; the results are in `docs/spikes/m4-quality-results.md`
("Round 2"). Outcome: the owner chose effort low on the Sonnet tier as Curia's default (D22), so
`FLAGS=()` now sends effort low, and a `--thinking` flag alone now comes with effort low too. To send
what round 2 sent, the blocks below therefore add `--effort default` (no effort setting) to the
default run and to the two runs without thinking. The text of this step describes the settings as
they were before D22.

A later sitting, once the revised advisor (plain words, about 120 words, bold only) is merged. It asks
the same five questions on Sonnet three times, once per setting, to find out how much of the wait
before the first word is the model's thinking, and whether the answers stay good without it. The 30
minutes include step 0 and reading and rating the 15 answers; the optional Haiku runs add about 15.
Round 1 results are in `docs/spikes/m4-quality-results.md`.

**What the settings mean.** Before it writes its first word, the model may think the question over.
Today Curia sends no thinking setting, so both models do that at their own default: Sonnet at effort
`high`, Haiku at effort `medium`. There is no separate thinking budget: the thinking shares the
answer limit of 4096 tokens and is billed like the answer (as output). Its text is not shown, so it
is probably most of the pause before the first word (round 1: about 6 seconds on Sonnet, against 1.3
seconds for the tiny check of step 1, whose prompt is far shorter); round 2 measures how much. The
two settings to try:

- `--effort low`: the model thinks less, and skips thinking on most simple questions;
- `--thinking between_tools` on Sonnet, `--thinking disabled` on Haiku: no thinking before the
  answer at all. Use the word given for each model: Sonnet refuses `disabled` (HTTP 400).

1. Do step 0 again in a new Terminal window: the build in 0.2 picks up the revised advisor (`git pull`
   brings it), then 0.3 (it sets `FLAGS=()`), the key (0.4) and 0.5 (Sonnet, transcripts on). Then
   copy the first block of step 2 once (it defines `ask_one` and `run_questions`), and set your own
   question again, the same text as in round 1:

   ```sh
   Q5='write your round 1 question here'
   ```

2. The default settings, then low effort, then no thinking. Each block is one run of five questions;
   wait for the five `exit code` lines before you copy the next block. The first question of a run
   may be a little slower again (a changed setting can make the service store the start of the prompt
   anew; the table's **Cache write** column shows it), so compare the medians rather than question 1:

   ```sh
   LABEL=sonnet2
   FLAGS=(--effort default)
   run_questions
   ask_one 5 "$Q5"
   ```

   ```sh
   LABEL=sonnet2-low
   FLAGS=(--effort low)
   run_questions
   ask_one 5 "$Q5"
   ```

   ```sh
   LABEL=sonnet2-off
   FLAGS=(--effort default --thinking between_tools)
   run_questions
   ask_one 5 "$Q5"
   ```

   Check that the settings were used (the first line of each run's first file; it also shows
   `"persona_version":3`, the revised advisor, in round 2, and 4 since the polish after it):

   ```sh
   grep -m1 '"event":"ready"' ~/curia_m4_results/runs/sonnet2*-q1.jsonl
   ```

   It prints one long line per run, starting with the file's path (ending in `sonnet2-low-q1.jsonl`
   and so on): the `sonnet2-low-q1.jsonl` line shows `"effort":"low"`, the `sonnet2-off-q1.jsonl`
   line `"thinking":"between_tools"`, and the `sonnet2-q1.jsonl` line `"effort":"default"` and
   `"thinking":"default"`.

3. Optional, if you want to see Haiku with the same two changes:

   ```sh
   export CURIA_MODEL=claude-haiku-5-5
   LABEL=haiku2-low
   FLAGS=(--effort low)
   run_questions
   ask_one 5 "$Q5"
   ```

   ```sh
   LABEL=haiku2-off
   FLAGS=(--effort default --thinking disabled)
   run_questions
   ask_one 5 "$Q5"
   ```

4. Read and rate the new answers as in step 2 (the transcripts now also name the setting, in the
   lines `effort:` and `thinking:`). Things to look for besides the usual: does it avoid technical
   words (snapshot, data, export)? Is it about 120 words (the table's **Words** column)? Besides the
   numbered list, is bold the only formatting? Does it say "probably" when it guesses, and "In general" when it explains a game
   rule?

5. The table, as in step 4 (round 1's rows are in it too; the new runs are the ones labelled
   `sonnet2`, `sonnet2-low`, `sonnet2-off` and, if you ran them, `haiku2-low`, `haiku2-off`):

   ```sh
   python3 ~/PycharmProjects/curia/tools/m4/quality_table.py ~/curia_m4_results/runs
   ```

   The **Run** column names the run (`sonnet2`, `sonnet2-low`, ...), the **Setting** column what it
   sent (`default`, `effort low`, `thinking between_tools`, ...), and the summary at the end gives
   the median times and words per run.

6. Step 6 (clean up).

**What to send back for round 2**, in one message: the round-2 rows of the table with your ratings
and short notes, the summary lines of those runs, and which setting you want as the default (one
sentence why). I then set that default in a small follow-up change.

## Cost

About **$1 in total**: the checks cost a few cents (the cache check about 3 cents), and each of the ten
questions sends the same few thousand tokens of prompt (the service charges less for the repeated part)
and receives an answer of a few hundred. Round 2 (step 7) is about 30 cents for the three Sonnet runs,
a little more with the optional Haiku runs. A $5 spending limit on the key in the service's console is a
good safety net. The exact figures are on the usage page of your account; you may send me what it
shows.

## About the macOS permission prompts

This sitting does not open a window and does not use any permission-protected feature (no
Accessibility, no Input Monitoring, no Screen Recording). The Terminal already has some of those
permissions, so macOS could attribute things to Terminal instead of to Curia; that does not matter for
this sitting because the program does not use them. If a macOS permission prompt appears anyway, press
**Don't Allow** and tell me the exact text and which app it names. How the finished window app is
started (from Terminal or by double-click) is decided at the game sitting, not here.

## Troubleshooting

| What you see | What it means and what to do |
|---|---|
| A line `curia: CURIA_... is missing or not usable` (and `"event":"not_configured"` in the file), exit code 2 | The named variable is not set in this Terminal window (or has a value the program refuses: the key needs 8 or more visible characters without spaces, the model only letters, digits and `. _ : -`). Repeat step 0.4 or 0.5 in this window, then run the same command again. Nothing was sent. |
| A line `curia: --log names a folder` or `the log file does not exist` | `$LOG` is wrong. Run `ls -l "$LOG"`; it must show one file of about 3 MB. |
| `fatal` with `snapshot_not_found` | `$SNAP` is not a snapshot number of this log. Use `SNAP=75` (or `SNAP=1`). |
| `[FAILED] Sign-in header ...: rejected (HTTP 401)` or an `error` event with `"code":"Auth"` | The service did not accept the key. Check that it was typed completely and still exists in the console; retype it (step 0.4). Then try `--check-api --auth x-api-key` once and tell me whether that one works. |
| `model was not found` / HTTP 404 | The model name is wrong. Stop and send me the exact name you typed. |
| HTTP 429 / `too many requests` | A limit was reached. Wait a minute and run the question again; if it repeats, look at the limits on the usage page. |
| HTTP 529 or `busy` | The service is overloaded. Try again in a few minutes. Not about your setup. |
| `could not be reached` / `Network` | No connection to the service. Check the internet connection, a VPN, a proxy or a firewall. |
| `command not found` / `no such file` for the program | The build in step 0.2 did not finish, or `$CURIA` was typed in another window. Rebuild and set `CURIA=` again in this window. |
| `zsh: no matches found`, `zsh: event not found` or odd quote characters | A line was copied with different quotes, or your own question (step 2) holds a `!`, a quote or an apostrophe. Copy again from this page, and keep your question free of `!`, `"` and `'`. |
| `ask_one: LABEL: set LABEL first` | You skipped the line `LABEL=sonnet` (or `LABEL=haiku`). Type it and run the questions again. |
| `set FLAGS first` | `FLAGS` is not set in this window. Type `FLAGS=()` (Curia's own settings; `FLAGS=(--effort default)` repeats the default runs of rounds 1 and 2) or the line the step shows, and run the questions again. |
| `curia: --effort needs ...` or `curia: --thinking needs ...`, exit code 2 | A setting in `FLAGS` is mistyped. Copy the `FLAGS=(...)` line from step 7 again. Nothing was sent. |
| Exit code `1` and `"code":"BadRequest"` (HTTP 400) in the file, with a setting from step 7 or with `FLAGS=()` (since D22 every run sends effort low) | The model does not accept that setting. With a setting from step 7 that is a result: write "not supported" in the notes for that run and go on with the next one. With `FLAGS=()`, run the same question once with `FLAGS=(--effort default)`; if that works, tell me. |
| An answer calls +9.2 a month the "net" income | Expected with snapshot 75 (mod 0.2.0 exported the income before expenses; see step 0.3). Rate the reasoning, not that number. |
| `command not found: ask_one` | The commands of step 2 are not defined in this window. Copy the first block of step 2 again. |
| Exit code `1` for a question | That question ended in an error; the file still has the details (`"event":"error"`). Run it again; tell me if it repeats. |
| Exit code `3` | The run took longer than 30 minutes and was stopped. Tell me. |
| Exit code `4` | An internal error. The last line of the file says `fatal`; send me that line. |
| An answer is cut off in the middle | The answer reached its length limit (`"max_tokens": true` in the file, `cut off` in the table). Note it; that is a result. |
