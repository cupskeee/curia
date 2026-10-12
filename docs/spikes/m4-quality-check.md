# M4 quality check (owner, macOS, no game, about 40 minutes, about $1)

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
   umask 077
   mkdir -p ~/curia_m4_results/runs
   chmod 700 ~/curia_m4_results ~/curia_m4_results/runs
   ls -l "$CURIA" "$LOG"
   ```

   `SNAP=75` chooses which saved moment of the realm the questions are about (see step 2).
   Both files (the program and the log) must be listed; if either is missing, stop and tell me which.
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
lines that start with `[ok]`, an empty line and the `CHECK_API_RESULT` line:

```
Sending one tiny request (usually a few seconds)...
[ok] API key: present (never shown)
[ok] Model: claude-sonnet-5-5
[ok] Connection and certificates: connected in 120 ms, certificates checked with the system default certificate store
[ok] Sign-in header (bearer): accepted (HTTP 200)
[ok] Tiny answer: first token after 900 ms, finished after 1200 ms; tokens in 30, out 5, stop reason end_turn

CHECK_API_RESULT ok=true key=present model=set tls=ok ca=default auth=bearer status=200 completion=ok ...
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
uses `SNAP=75` (set in step 0.3): a king in 1071 with 928 gold, 11 vassals of whom several dislike him,
a faction against him and a war. If you want to see the other one, set `SNAP=1` and repeat the run
with another label; write the snapshot number next to your ratings (the table has a column for it).

Copy this block once. It only defines two commands and runs nothing:

```sh
ask_one() {
  : "${LABEL:?set LABEL first, for example LABEL=sonnet}"
  echo "question $1: asking (10 to 60 seconds is normal, nothing is shown until it ends) ..."
  printf 'ask %s\n' "$2" | "$CURIA" --headless-chat --log "$LOG" --from-start --frame "$SNAP" > ~/curia_m4_results/runs/$LABEL-q$1.jsonl
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

This prints a table with one row per question and model: the snapshot number, time to the first token,
total time, tokens, the cache numbers and how the answer ended, plus empty columns for your rating and
notes, and then the median times per model. The rows are in the order of the file names (`haiku-...`
comes before `sonnet-...`); each row names its model and question, so the order does not matter. Copy
it into a text note and fill in the two last columns (the table below is the shape of what you send
back):

| Model | Question | First token (ms) | Total (ms) | Rating 1-5 | Notes |
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
unset CURIA_ANTHROPIC_API_KEY CURIA_MODEL CURIA_LOG_CONTENT Q5
unset -f ask_one run_questions
grep -l "sk-ant-" ~/curia_m4_results/runs/* ~/curia_m4_results/transcripts/* 2>/dev/null || echo "no key found in the result files"
```

The last line must say `no key found in the result files`. If it lists a file, do not open or share
it; tell me. Closing the Terminal window also forgets the key. The files in `~/curia_m4_results/`
stay on your computer; they hold game text, which is why they are never inside the repository.

## Cost

About **$1 in total**: the checks cost a few cents (the cache check about 3 cents), and each of the ten
questions sends the same few thousand tokens of prompt (the service charges less for the repeated part)
and receives an answer of a few hundred. A $5 spending limit on the key in the service's console is a
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
| `command not found: ask_one` | The commands of step 2 are not defined in this window. Copy the first block of step 2 again. |
| Exit code `1` for a question | That question ended in an error; the file still has the details (`"event":"error"`). Run it again; tell me if it repeats. |
| Exit code `3` | The run took longer than 30 minutes and was stopped. Tell me. |
| Exit code `4` | An internal error. The last line of the file says `fatal`; send me that line. |
| An answer is cut off in the middle | The answer reached its length limit (`"max_tokens": true` in the file, `cut off` in the table). Note it; that is a result. |
