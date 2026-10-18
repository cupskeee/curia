<!-- SPDX-License-Identifier: MIT -->
# Milestone 4 tools

- `quality_table.py` reads the JSON-lines output of `curia --headless-chat` (the files the quality
  sheet, `docs/spikes/m4-quality-check.md`, tells the owner to save) and prints a markdown table,
  one row per question: run label (the file name without `-q<n>` and the extension), model, setting
  (the `--effort` / `--thinking` values the run sent, from the `done` event, else the `ready` event;
  `default` when none was sent; since D22 a run without flags shows `effort low`, Curia's default,
  and a run with `--effort default` shows `default`), question, snapshot number, time to the first token, total time,
  tokens, words (the answer's text split at white space), cache numbers, how the answer ended, and
  empty columns for the rating and notes; then, per run label and setting, the median times and
  words. It never prints an answer. `--csv` gives comma-separated values.

  ```
  python3 tools/m4/quality_table.py ~/curia_m4_results/runs
  ```

- `analyze_applog.py` reads Curia's app log (`curia.log`; a folder is read as `curia.log.1` then
  `curia.log`; a path is required: it never reads an app log it was not given) and prints the markdown tables of the
  game sittings (`docs/spikes/m4-session-sheet.md`): the runs (one per start of Curia: window and
  focus mode, which development variables arrived, tray, hot key, settings), the readings
  (`frame_complete`: game date, gold, income in tenths, milliseconds until `overlay_shown` and the
  placement basis), the income across month boundaries (S3: a verdict per series of readings at
  most 3 game days apart, and over all readings; "not shown" when the income changed nowhere), the
  questions (first token, total, tokens, cache, how they ended, the request's HTTP version and
  connection), the cache pairs (a second question within 5 minutes on the same reading), the stops
  (Stop to the request's end, the client's connection word, the next request's new connections and
  the seconds before it), bytes per hour of `debug.log` while CK3 ran (from `log_bytes`,
  `ck3_running` and `log_truncated`, a launch's lines up to 6 s before `ck3_running` included; per
  hour and per stretch, with and without Curia's lines, and "play only": after a launch, from the
  count before the first reading, or 10 minutes after the launch without one), the overlay's
  evidence (`overlay_shown`, `overlay_focus`, `overlay_keys`, `overlay_placed`, `overlay_opened`),
  CK3 and the notices, and a key check: lines shaped like an API key (`sk-ant-...`, or a long token
  of mixed letters and digits), by line number only. It prints numbers and the app's fixed words
  only: a field value that is not one of the words the app writes there shows as `other`.
  `--timeline` also lists the overlay, reading, CK3 and question lines in time order; `--utc` shows
  UTC instead of the local time. Exit 0, 1 if a key-shaped line was found, 2 if no app log could be
  read (a usage error) or on an internal error (a fixed message, nothing on stdout). The fields it
  relies on (keep them; add fields rather than rename):
  `frame_complete` `reading`, `snap`, `mode`, `game_date=YYYY-MM-DD`, `gold`, `income_tenths`
  (`none` without the FIN section); `ask_started` `gen`, `reading`; `ask_first_token`, `ask_done`
  (usage, `first_token_ms`, `total_ms`), `ask_cancelled`, `ask_error`; `net_done` `outcome`, `http`,
  `connects`, `conn`; `log_bytes` `why=minute|exit`, `read`, `curia`, `mailbox_dropped` (cumulative
  since Curia started); `ck3_running`, `log_truncated`, `notice`, `dev_config`, `window_start`,
  `controls`, `provider_set` and the `overlay_*` lines.

  ```
  python3 -I tools/m4/analyze_applog.py ~/curia_m4_results/sitting-a/applog
  ```

- `grep_key.py` looks for the API key in files without showing it. It reads the key from
  `CURIA_ANTHROPIC_API_KEY` in the shell that runs it (never from the command line or a file; a key
  typed among the arguments is refused) and scans the files and folders given (recursively, binary
  safe, at most `--max-bytes` per file, 256 MiB by default, with a note for a file read only in part;
  links inside folders are not followed; special files are skipped; `--name-contains TEXT` limits
  the files inside folders by name) for the key as it is, JSON-escaped, URL-encoded, in base64
  (three alignments, both alphabets) and in UTF-16, for its first 24 characters as they are (a cut
  copy; keys of 32 characters or more), and counts the generic shape `sk-ant-...` separately (test
  keys that say FAKE are summarised). It does not look inside compressed files or into base64 split
  over lines. It prints only paths and counts, never the key, a part of it or the text around a
  match, hides a path that contains the key, and prints the home folder as `~`. Exit 0 if the key
  was found nowhere, 1 if it (or its first characters) was found, 2 for a usage error (the variable
  unset or shorter than 8 characters, no path exists) or an internal error, 3 if it was not found
  but a file or folder could not be read or a file was read only in part (not complete).

  ```
  python3 -I tools/m4/grep_key.py ~/curia_m4_results ~/Library/Logs/Curia
  ```

- Tests: `python3 -m unittest discover -s tools/m4` (CI job `lint`, step "M4 tools unit tests").
  Standard library only, Python 3.10 or later, safe to run with `python3 -I`; synthetic data and
  FAKE keys in temporary folders only.
