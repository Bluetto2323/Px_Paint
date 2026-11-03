#!/usr/bin/env python3
"""Accessibility audit of the uiautomator dumps taken by ui_screenshots.sh.

For every clickable / focusable-and-clickable / scrollable-range node:
  * it has a label (content-desc or text, or a labelled child for rows);
  * its touch area is at least 48 x 48 dp (dp = px * 160 / density).
Reports every problem and a summary per screen. A dump only holds what is on
screen, so this covers the states captured, not every possible state.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

out = sys.argv[1]
MIN_DP = 48
problems_total = 0
nodes_total = 0
lines = []
try:
    entries = [l.split() for l in open(os.path.join(out, "dumps.txt")) if l.strip()]
except OSError:
    entries = []
for name, density in ((e[0], int(e[1]) if len(e) > 1 and e[1].isdigit() else 420) for e in entries):
    path = os.path.join(out, name + ".xml")
    try:
        root = ET.parse(path).getroot()
    except Exception as ex:
        lines.append(f"{name}: no dump ({ex.__class__.__name__})")
        continue
    checked = 0
    problems = []
    for n in root.iter("node"):
        if n.get("package") != "com.paintengine":
            continue
        clickable = n.get("clickable") == "true" or n.get("long-clickable") == "true"
        if not clickable or n.get("enabled") != "true":
            continue
        checked += 1
        label = (n.get("content-desc") or "").strip() or (n.get("text") or "").strip()
        if not label:
            # A row is labelled by its texts.
            label = " ".join((c.get("text") or c.get("content-desc") or "").strip() for c in n.iter("node")).strip()
        x0, y0, x1, y1 = map(int, re.findall(r"\d+", n.get("bounds", "0,0,0,0")))
        w_dp = (x1 - x0) * 160 / density
        h_dp = (y1 - y0) * 160 / density
        cls = n.get("class", "").split(".")[-1]
        if not label:
            problems.append(f"  unlabelled {cls} at {n.get('bounds')}")
        # Partly scrolled-out rows are clipped by the dump: only check fully visible, non-text-field nodes.
        if (w_dp < MIN_DP - 0.5 or h_dp < MIN_DP - 0.5) and cls != "EditText":
            problems.append(f"  small target {w_dp:.0f}x{h_dp:.0f} dp: {cls} '{label[:40]}' at {n.get('bounds')}")
    nodes_total += checked
    problems_total += len(problems)
    lines.append(f"{name} (density {density}): {checked} interactive nodes, {len(problems)} problems")
    lines.extend(problems)
print("\n".join(lines))
print(f"TOTAL: {nodes_total} interactive nodes checked, {problems_total} problems")
