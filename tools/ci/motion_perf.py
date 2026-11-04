#!/usr/bin/env python3
"""Summarises `dumpsys gfxinfo <pkg>` outputs (UI thread frame times of the
app's window while a scripted interaction ran) into one table.
usage: motion_perf.py <out dir> <label>=<file> ..."""
import os
import re
import sys

out = sys.argv[1]
rows = []
for arg in sys.argv[2:]:
    label, name = arg.split("=", 1)
    try:
        text = open(os.path.join(out, name)).read()
    except OSError:
        rows.append(f"{label}: no data")
        continue
    def grab(pattern):
        m = re.search(pattern, text)
        return m.group(1) if m else "?"
    rows.append(
        f"{label}: frames {grab(r'Total frames rendered: (\d+)')}, janky {grab(r'Janky frames: (\d+ \([\d.]+%\))')}, "
        f"p50 {grab(r'50th percentile: (\d+ms)')}, p90 {grab(r'90th percentile: (\d+ms)')}, "
        f"p95 {grab(r'95th percentile: (\d+ms)')}, p99 {grab(r'99th percentile: (\d+ms)')}, "
        f"slow UI thread {grab(r'Number Slow UI thread: (\d+)')}")
print("\n".join(rows))
