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

- Tests: `python3 -m unittest discover -s tools/m4` (CI job `lint`, step "M4 tools unit tests").
  Standard library only; synthetic data.
