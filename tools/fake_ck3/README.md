# Fake CK3

A stand-in for the game's log writing, so that the log watcher, the parser and the whole chain up
to the golden events can be tested without Crusader Kings III (`docs/architecture.md` section 7,
item 1). Python 3 standard library only; it works on Windows, macOS and Linux (binary files, no
`fcntl`).

It creates a temporary CK3-style user folder and appends to `logs/debug.log`:

- synthetic **noise lines** and multi-line **error entries**, every line with an engine-style
  prefix of 119 to 131 bytes before the marker. The prefix copies only the *shape* seen in the M3
  sessions; its words are invented and it is built at run time, so no tracked file holds an engine
  log line (`tools/ci/scan_patterns.sh` checks that). Raw captures stay under the gitignored
  `reference/`;
- **CURIA1 frames**: the example frame of `docs/wire-format.md` section 4 (artificial names), or any
  fixture of `tests/fixtures/curia1/` (directives `!noise`, `!raw` and `!truncate` are honoured; bare
  records get the prefix).

## Command line

```
python3 tools/fake_ck3/fake_ck3.py [--root DIR] [--layout plain|wine] [flags]
```

The first line printed is `LOG <path>`; the last is `DONE <bytes>`. Without `--root` the folder is
temporary and removed on exit (`--keep` keeps it).

| Flag | Effect |
|---|---|
| `--layout wine` | the nested path of a Wine/Proton prefix (`.../pfx/drive_c/users/steamuser/Documents/Paradox Interactive/Crusader Kings III/logs/debug.log`, spaces included); default `plain`: `<root>/logs/debug.log` |
| `--fixture FILE` (repeatable) | play fixtures instead of the built-in frame |
| `--repeat N`, `--interval S` | N frames (or fixture rounds), S seconds apart |
| `--snap-start N` | snap of the first generated frame (it counts up) |
| `--noise N` | N noise lines before the frames |
| `--line-buffer` | write each line in two chunks, torn two bytes before its end (the first chunk of `...\|FIN\|312\|78` is `...\|FIN\|312\|7`), so a reader that parses a torn line gets a wrong number; the library API has an `after_first_chunk` hook so a test can make the tear deterministic |
| `--flush-delay S` | hold every write back for S seconds |
| `--burst` | all lines of a frame in one write (default: one write per line) |
| `--prior-frames N`, `--launch-delay S`, `--truncate-at-launch` | N frames of an earlier session, then (after S seconds, in which a reader can attach) empty the log **in place** (same file, size back to 0), as the game does at launch (M3 T5) |
| `--partial-frame` | one more frame that stops before `END` |
| `--interleave` | noise lines and multi-line error entries inside frames |
| `--die-after-bytes N` | stop writing for good after exactly N bytes (the cut may fall inside a line): the log cap |
| `--fsync` | `os.fsync` after every write |
| `--gold N`, `--income-tenths N`, `--date YYYY-M-D`, `--heir NAME`, `--heir-opinion N`, `--mod-version V`, `--ruler NAME` | the values of the generated example frame (`FIN`, `BEGIN`, `HEIR`); the defaults are the example frame of `docs/wire-format.md` section 4, so every existing test is unchanged. Synthetic data only; the date must exist in the game's calendar (no leap years: 28 February is the last day of February); a name is 1 to 64 characters without `|` or control characters, the version a token of letters, digits and `. _ - +`. The library takes the same through `FakeCk3(frame_params=FrameParams(...))` (or `fake.frame_params = ...` between frames) |
| `--stdin` | after the workload, read commands: `frame`, `partial`, `rest` (finish a partial frame), `noise [n]`, `error`, `truncate`, `fixture <path>`, `size`, `quit` |

## Library

```python
import fake_ck3 as fk

with fk.FakeCk3(burst=True, interleave=True) as fake:
    fake.launch()                    # creates logs/debug.log
    fake.append_frame(snap=7)        # the example frame
    fake.append_frame(snap=8, partial=True)
    fake.finish_frame()
    fake.launch(truncate=True)       # the next game launch
    fake.play_fixture("tests/fixtures/curia1/damaged_truncated.log",
                      between=lambda kind: ...)   # 'truncate' / 'truncated' hooks
```

## Tests

- `python3 -m unittest discover -s tools/fake_ck3` runs the unit tests of the fake itself
  (`test_fake_ck3.py`; CI job `lint`).
- `e2e_test.py` is the end-to-end test: for each scenario it starts the fake and the built
  `curia --watch-log <log>` (headless: no window, one JSON line per event), drives the fake and
  compares the printed events with the golden `*.events.json` of the matching fixture. Only event
  kind, snap, reason and frames are compared, never the human-readable `detail`. Every scenario
  ends with a good sentinel frame, so curia's exit after N complete frames proves nothing was lost
  behind the scenario. Scenarios: basic frame, interleaved noise, burst and per-line writes, torn
  lines, delayed flush, partial frame then the rest, truncation at launch (between frames and with
  an open frame), oversize name, damaged frame, two frames in a row, attach to an existing log
  (history not replayed), a log that appears later, `LOAD` lines, a Wine layout path, and every
  fixture of `tests/fixtures/curia1/`. CTest runs it as `e2e_fake_ck3` (label `e2e`; needs the app
  build and Python 3):

  ```
  python3 tools/fake_ck3/e2e_test.py --curia build/macos-arm64/app/curia --fixtures tests/fixtures/curia1
  ```

  curia reports on stderr how far it has read (`curia: offset <n> ...`) and what happened to the file
  (`curia: log attached | truncated | missing | ...`); the test uses these to know when the tail has
  caught up, instead of sleeping.
- The chain with the mock LLM (fake CK3 -> `curia --headless-chat` -> `tools/mock_llm`) is
  `app/tests/chat_e2e.py` (ctest `chat_e2e` and `e2e_secrecy`): it writes frames with chosen gold, heir
  and date through the parameters above, and uses `--die-after-bytes` for the silent log.

## Limits

- The simulator matches the **structure** observed in M3, not the game: it cannot tell whether the
  real game flushes differently, nor how Windows file sharing behaves with the real game's handle
  (unverified; `docs/wire-format.md` section 9, `architecture.md` section 5).
- A truncation followed by growth beyond the old size between two polls cannot be detected by the
  tailer (no inode or mtime check); the end-to-end test lets the reader notice a truncation before it
  writes more.
