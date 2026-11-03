#!/usr/bin/env python3
"""Prints "x y" (centre) of the first node in a uiautomator dump whose
content-desc starts with the given text; exit 1 if there is none."""
import re
import sys
import xml.etree.ElementTree as ET

try:
    root = ET.parse(sys.argv[1]).getroot()
except Exception:
    sys.exit(1)
for n in root.iter("node"):
    if n.get("content-desc", "").startswith(sys.argv[2]) and n.get("enabled") == "true":
        x0, y0, x1, y1 = map(int, re.findall(r"\d+", n.get("bounds", "")))
        print((x0 + x1) // 2, (y0 + y1) // 2)
        sys.exit(0)
sys.exit(1)
