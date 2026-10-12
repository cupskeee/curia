<!-- SPDX-License-Identifier: MIT -->
# Milestone 4 tools

- `quality_table.py` reads the JSON-lines output of `curia --headless-chat` (the files the quality
  sheet, `docs/spikes/m4-quality-check.md`, tells the owner to save) and prints a markdown table, one
  row per question: model, question, time to the first token, total time, tokens, cache numbers, how
  the answer ended, and empty columns for the rating and notes; then the median times per model. It
  never prints an answer. `--csv` gives comma-separated values.

  ```
  python3 tools/m4/quality_table.py ~/curia_m4_results/runs
  ```

- Tests: `python3 -m unittest discover -s tools/m4` (CI job `lint`, step "M4 tools unit tests").
  Standard library only; synthetic data.
