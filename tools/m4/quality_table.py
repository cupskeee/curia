#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turns the JSON-lines output of `curia --headless-chat` into a markdown table.

The owner's quality sitting (docs/spikes/m4-quality-check.md) saves the output of each run into a
file. This reads those files (or folders of *.jsonl files) and prints one row per question: the
model, the question, the snapshot number, the time to the first token, the total time, the tokens, the cache numbers and
how the answer ended, plus empty columns to fill in (rating 1 to 5, notes), and a short summary per
model (median times). It needs only the numbers and the question text; it never prints an answer.

    python3 tools/m4/quality_table.py ~/curia_m4_results/runs
    python3 tools/m4/quality_table.py run1.jsonl run2.jsonl --csv

Standard library only. Tolerant on purpose: a line that is not JSON, an event without a field, a
run that ended early or a file that cannot be read is skipped or shown as "-", never a crash.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


def read_events(path: str) -> List[Dict[str, Any]]:
    events: List[Dict[str, Any]] = []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    value = json.loads(line)
                except ValueError:
                    continue
                if isinstance(value, dict):
                    events.append(value)
    except OSError:
        return []
    return events


def expand_paths(paths: Sequence[str]) -> List[str]:
    files: List[str] = []
    for path in paths:
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                if name.endswith((".jsonl", ".json", ".txt", ".log")):
                    files.append(os.path.join(path, name))
        else:
            files.append(path)
    return files


def number(value: Any) -> Optional[int]:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return int(value)


def rows_from_events(events: Iterable[Dict[str, Any]], source: str = "") -> List[Dict[str, Any]]:
    """One row per question that was asked in a run."""
    model = "-"
    snap: Optional[int] = None
    rows: List[Dict[str, Any]] = []
    current: Optional[Dict[str, Any]] = None
    for event in events:
        if not isinstance(event, dict):
            continue
        kind = event.get("event")
        if kind == "ready":
            model = str(event.get("model") or "-")
            snap = number(event.get("snapshot"))
        elif kind == "ask":
            current = {"source": source, "model": model, "snap": snap, "question": str(event.get("text") or "-"),
                       "generation": number(event.get("generation")), "first_token_ms": None,
                       "total_ms": None, "input": None, "output": None, "cache_read": None,
                       "cache_write": None, "result": "no result", "stop_reason": "", "chars": None}
            rows.append(current)
        elif current is not None and kind in ("done", "cancelled", "error"):
            current["total_ms"] = number(event.get("total_ms"))
            if kind == "done":
                usage = event.get("usage") if isinstance(event.get("usage"), dict) else {}
                current["first_token_ms"] = number(event.get("first_token_ms"))
                for name in ("input", "output", "cache_read", "cache_write"):
                    current[name] = number(usage.get(name))
                current["stop_reason"] = str(event.get("stop_reason") or "")
                current["result"] = "cut off (max_tokens)" if event.get("max_tokens") else "ok"
                current["chars"] = len(event["text"]) if isinstance(event.get("text"), str) else None
            elif kind == "cancelled":
                current["result"] = "stopped"
            else:
                current["result"] = "error " + str(event.get("code") or "?")
                current["chars"] = number(event.get("partial_len"))
            current = None
    return rows


def show(value: Any) -> str:
    return "-" if value is None else str(value)


COLUMNS = ("Model", "Question", "Snap", "First token (ms)", "Total (ms)", "In", "Out", "Cache read",
           "Cache write", "Result", "Rating 1-5", "Notes")


def row_cells(row: Dict[str, Any]) -> List[str]:
    return [row["model"], row["question"].replace("|", "/"), show(row["snap"]),
            show(row["first_token_ms"]),
            show(row["total_ms"]), show(row["input"]), show(row["output"]), show(row["cache_read"]),
            show(row["cache_write"]), row["result"], "", ""]


def median(values: List[int]) -> Optional[int]:
    return int(statistics.median(values)) if values else None


def summary_rows(rows: List[Dict[str, Any]]) -> List[Tuple[str, int, Optional[int], Optional[int], int]]:
    """(model, questions, median first token, median total, answers that failed) per model."""
    order: List[str] = []
    for row in rows:
        if row["model"] not in order:
            order.append(row["model"])
    out = []
    for model in order:
        mine = [r for r in rows if r["model"] == model]
        firsts = [r["first_token_ms"] for r in mine if r["first_token_ms"] is not None]
        totals = [r["total_ms"] for r in mine if r["total_ms"] is not None and r["result"].startswith(("ok", "cut"))]
        failed = sum(1 for r in mine if r["result"] not in ("ok",) and not r["result"].startswith("cut"))
        out.append((model, len(mine), median(firsts), median(totals), failed))
    return out


def markdown(rows: List[Dict[str, Any]]) -> str:
    lines = ["| " + " | ".join(COLUMNS) + " |", "|" + "|".join("---" for _ in COLUMNS) + "|"]
    for row in rows:
        lines.append("| " + " | ".join(row_cells(row)) + " |")
    lines.append("")
    lines.append("| Model | Questions | Median first token (ms) | Median total (ms) | Not ok |")
    lines.append("|---|---|---|---|---|")
    for model, count, first, total, failed in summary_rows(rows):
        lines.append("| %s | %d | %s | %s | %d |" % (model, count, show(first), show(total), failed))
    return "\n".join(lines) + "\n"


def csv_text(rows: List[Dict[str, Any]]) -> str:
    def cell(text: str) -> str:
        return '"' + text.replace('"', '""') + '"'
    lines = [",".join(cell(c) for c in COLUMNS)]
    for row in rows:
        lines.append(",".join(cell(c) for c in row_cells(row)))
    return "\n".join(lines) + "\n"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Markdown table from curia --headless-chat output files.")
    parser.add_argument("paths", nargs="+", help="output files, or folders of them")
    parser.add_argument("--csv", action="store_true", help="comma-separated values instead of markdown")
    args = parser.parse_args(argv)
    rows: List[Dict[str, Any]] = []
    for path in expand_paths(args.paths):
        rows.extend(rows_from_events(read_events(path), os.path.basename(path)))
    if not rows:
        print("No questions found in the given files.", file=sys.stderr)
        return 1
    sys.stdout.write(csv_text(rows) if args.csv else markdown(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
