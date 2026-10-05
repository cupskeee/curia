<!-- SPDX-License-Identifier: MIT -->
# Milestone 3 log watcher

`logwatch.py` watches CK3's `debug.log` and `error.log` while the game runs and measures what the M3 spike
needs: flush latency of the `CURIA1|` marker lines (T3), line format, length and charset (T4), truncation at
launch (T5), bytes per export and bytes appended per hour (T8), and whether anything mentioning the mod
lands in `error.log` (lines matching `curia[_/]`, case-insensitive). Python 3.9+, standard library only. It only reads the logs; it never writes to the game
folders.

## Run (macOS)

```
python3 tools/m3/logwatch.py
```

1. Start it **before** launching CK3. It prints `waiting for debug.log` until the file exists, and then
   records the launch itself (the file being recreated or truncated is the T5 answer).
2. While it runs, type a **label** and Enter to tag what the game is doing: `paused`, `menu`, `fast`,
   `normal`, and so on. Every CURIA line is filed under the label current when it arrived. A line starting
   with `!` is stored as a free-text note (for example `!clicked Advisor twice`).
3. Press Ctrl+C when done (or use `--duration <seconds>`). The summary is printed and saved. Closing the
   Terminal window (SIGHUP), SIGTERM or an unexpected error also ends the run through the same path: one last
   poll, then the summary and the files are written.

Options: `--log-dir`, `--out-dir`, `--interval-ms` (default 10), `--duration`, `--report-every` (seconds,
default 30, 0 = off), `--from-start` (read content already in the files as backlog, which is excluded from the
latency and budget numbers; by default the watcher attaches at the end of an existing file),
`--count-convention all|body` (does the `END` line count include `BEGIN` and `END`; both results are always
recorded), `--long-line-bytes`, `--engine-offset-s`, `--mod-pattern`, `--label`, `--no-stdin`, `--quiet`,
`--selftest`.

`--engine-offset-s` is **added** to the engine clock before the bounds are computed, so an engine clock that runs
ahead of the Mac needs a negative value (`-3600` for one hour ahead); fractions of a second are kept.
`--mod-pattern` is a case-insensitive regular expression (default `curia[_/]`) that decides which `error.log`
lines count as mentioning the mod; the pattern in use is printed in the summary.

## Outputs

Written to `~/curia_m3_results/` (`--out-dir`), one set per run, named by start time:

- `watch-<stamp>.jsonl`: one JSON object per event (`sample`, `frame`, `truncation`, `file`, `label`, `note`,
  `status`, `error_hit`, `anomaly`, `budget`, `start`, `end`).
- `raw-curia-<stamp>.txt`: the CURIA1 lines as seen in the log, one per line, each with the line terminator that
  was seen (LF or CRLF), so the file is byte-faithful (the source for fixtures). A line emitted early because the
  partial-line buffer cap was hit has no terminator.
- `summary-<stamp>.txt`: the plain-text summary, also printed on exit.

These files contain the engine's own text (script paths, on_action names). They stay local and are never
committed; a fixture is made by hand from the marker lines with the engine prefix removed.

## How the latency numbers are meant

The engine stamps its lines with whole seconds, so one sample can only bracket the delay:
`lower = arrival - (engine second + 1)`, `upper = arrival - engine second`. Negative bounds are normal when a
line arrives within the same second. The summary states what the bounds permit per label and never turns them
into an estimate. Arrival is the moment the poll saw the new bytes, so up to one poll interval is included;
the click time is not measured, only the engine timestamp of the `debug_log` call. If the engine clock and the
Mac's clock disagree (timezone, skew), the summary reports the apparent offset instead of hiding it;
`--engine-offset-s` shifts the engine clock before the bounds are computed. Midnight rollover is handled.

Also recorded per CURIA line: bytes with and without the engine prefix, how many reads it took to arrive
complete, CRLF terminators, non-ASCII or control bytes (with offsets), invalid UTF-8, duplicates, lines over
4000 bytes, and the file's modification time at the moment it was read.

The byte rates (T8) are the appended totals divided by the **active window**, the span from the first appended
byte to the latest one, so idle time before the game started writing is not counted; the per-minute buckets use
the same first byte as their origin. The summary prints the watch's total elapsed time next to the active window.

Truncation, re-creation or disappearance of `debug.log` starts a new session: frames still open are closed and
reported as partial, and the duplicate and finished-frame checks start over (frames carry a `session` number in
the `.jsonl` file). The BOM answer (T4) is taken from the first bytes of the file whenever they arrive, also when
they are written after the poll that noticed the new or truncated file.

Frames are grouped by `snap_id` (second field after the marker). A frame is complete when it has `BEGIN`, an
`END` and the number stated by `END` matches the lines seen. Lines of other output between frame lines are
counted as interleaving. Frames without an `END` at exit are reported as partial.

## Limits

- A truncation followed by regrowth beyond the old size between two polls, on the same inode, cannot be seen.
- `error.log` hits are matched with the regular expression `curia[_/]`, case-insensitively (`--mod-pattern`
  changes it). Plain `curia` is not used because it also occurs inside other words. A message that names the
  mod some other way is not matched by the default.
- Windows and Linux log behaviour is not measured here (macOS only); the tool takes `--log-dir` for those
  platforms' folders.

## Tests

```
python3 -m unittest discover -s tools/m3 -v
python3 tools/m3/logwatch.py --selftest
```

The unit tests (the command CI runs) use synthetic data, an injected fake clock and explicit poll calls, so no
test depends on real timing. `--selftest` is a manual command: it runs a simulated writer thread against a
temporary folder with real timing and checks that the measured arrival falls within a tolerance of the write
time and that the true latency lies inside the computed bounds; it retries once if the first run fails. It is
not part of the unit test run because it depends on the machine's scheduling. Neither needs CK3 or the real
log folder.
