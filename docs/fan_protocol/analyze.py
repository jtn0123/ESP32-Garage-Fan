#!/usr/bin/env python3
"""Analyze a sigrok CSV dump: per-channel duty, edges, periods, pulse widths."""

from collections import Counter
import subprocess
import sys

SR_FILE = sys.argv[1]
SAMPLE_RATE = 1_000_000 if len(sys.argv) < 3 else int(sys.argv[2])

out = subprocess.run(
    ["sigrok-cli", "-i", SR_FILE, "-O", "csv"],
    capture_output=True,
    text=True,
    check=True,
).stdout

lines = [ln for ln in out.splitlines() if ln and not ln.startswith(";")]
header = lines[0].split(",")
rows = lines[1:]
n = len(rows)
chans = {name: [] for name in header}
data = [[] for _ in header]
for r in rows:
    for i, v in enumerate(r.split(",")):
        data[i].append(int(v))

names = {"D0": "D+", "D1": "D-", "D2": "V+"}
print(f"file={SR_FILE} samples={n} rate={SAMPLE_RATE/1e6:.0f}MHz dur={n/SAMPLE_RATE*1000:.0f}ms")
for i, name in enumerate(header):
    sig = data[i]
    label = names.get(name.strip(), name)
    high = sum(sig)
    duty = high / n * 100
    # edges
    edges = [j for j in range(1, n) if sig[j] != sig[j - 1]]
    print(f"\n{label}: duty={duty:.1f}% edges={len(edges)}")
    if not edges:
        print(f"  flat at {'HIGH' if sig[0] else 'LOW'}")
        continue
    # periods between rising edges
    rising = [j for j in edges if sig[j] == 1]
    if len(rising) > 2:
        periods = [b - a for a, b in zip(rising, rising[1:])]
        pc = Counter(periods).most_common(4)
        freq = SAMPLE_RATE / (sum(periods) / len(periods))
        print(
            f"  approx freq={freq:.1f}Hz  common periods(us): "
            + ", ".join(f"{p/SAMPLE_RATE*1e6:.1f}({c}x)" for p, c in pc)
        )
    # pulse widths (high and low)
    for level, tag in ((1, "high"), (0, "low")):
        widths = []
        start = None
        for j in range(n):
            if sig[j] == level and start is None:
                start = j
            elif sig[j] != level and start is not None:
                widths.append(j - start)
                start = None
        if widths:
            wc = Counter(widths).most_common(4)
            print(
                f"  {tag} widths(us): " + ", ".join(f"{w/SAMPLE_RATE*1e6:.1f}({c}x)" for w, c in wc)
            )
