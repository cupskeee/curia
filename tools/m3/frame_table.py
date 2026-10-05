#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tabulate the frames of a logwatch.py run (the .jsonl file): one row per frame, the number of complete
frames per label and button, and min / median / max of lines and bytes per button.

Usage: python3 tools/m3/frame_table.py [watch-*.jsonl]   (default: the newest file in ~/curia_m3_results)
Reads only the watcher's own output; prints counts and sizes, never game text.
"""
from __future__ import annotations

import collections
import glob
import json
import os
import statistics
import sys
from typing import Dict, Iterable, List, Optional


def load_frames(lines: Iterable[str], skipped: Optional[List[int]] = None) -> List[Dict]:
    """Frame records from a watcher .jsonl stream, each tagged with the button that produced it.

    A line that is not valid JSON or not a JSON object (for example a truncated last line of a run that
    was killed) is skipped, and counted in skipped[0] when a list is given."""
    frames = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except ValueError:
            rec = None
        if not isinstance(rec, dict):
            if skipped is not None:
                skipped[0] += 1
            continue
        if rec.get("type") == "frame":
            rec["button"] = "probes" if "PROBE" in rec.get("kinds", []) else "advisor"
            frames.append(rec)
    return frames


def _plain(value: object, limit: int = 40) -> str:
    """Print the snap number and the label as short plain text (a label is whatever the owner typed)."""
    return str(value)[:limit]


def format_report(frames: List[Dict]) -> str:
    out = ["snap | button | label | lines | END says | text bytes | file bytes | longest line | span s | complete"]
    for r in frames:
        out.append(" | ".join(_plain(r.get(k)) for k in (
            "snap_id", "button", "label", "lines", "declared_count", "payload_bytes",
            "raw_bytes_with_prefix", "longest_line_bytes", "span_s", "complete")))
    out.append("")
    out.append("complete frames per label and button (compare with the click tally):")
    counts = collections.Counter((r.get("label"), r["button"]) for r in frames if r.get("complete"))
    for (label, button), n in sorted(counts.items(), key=lambda kv: (str(kv[0][0]), kv[0][1])):
        out.append(f"  {_plain(label)} {button} {n}")
    out.append("")
    out.append("per button, complete frames (min / median / max):")
    for button in ("advisor", "probes"):
        rows = [r for r in frames if r.get("complete") and r["button"] == button]
        for key, name in (("lines", "lines"), ("payload_bytes", "text bytes"), ("raw_bytes_with_prefix", "file bytes")):
            values = [r[key] for r in rows if key in r]
            if values:
                out.append(f"  {button} {name}: {min(values)} / {statistics.median(values)} / {max(values)} (n={len(values)})")
    return "\n".join(out)


def newest_watch_file(results_dir: str) -> Optional[str]:
    files = glob.glob(os.path.join(results_dir, "watch-*.jsonl"))
    return max(files, key=os.path.getmtime) if files else None


def main(argv: List[str]) -> int:
    path = argv[1] if len(argv) > 1 else newest_watch_file(os.path.expanduser("~/curia_m3_results"))
    if not path or not os.path.exists(path):
        print("no watch-*.jsonl file found (pass the path as the first argument)", file=sys.stderr)
        return 2
    skipped = [0]
    with open(path, encoding="utf-8", errors="replace") as handle:
        frames = load_frames(handle, skipped)
    print(f"file: {path}")
    if skipped[0]:
        print(f"skipped {skipped[0]} unreadable line(s)")
    print(format_report(frames))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
