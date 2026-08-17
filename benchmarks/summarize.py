#!/usr/bin/env python3
"""Collapses the raw per-run rows into medians and prints markdown tables.

    benchmarks/summarize.py [benchmarks/raw.csv] [measured-seconds-per-run]
"""
import csv
import statistics
import sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "benchmarks/raw.csv"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

runs = defaultdict(list)
with open(path) as f:
    for row in csv.DictReader(f):
        runs[(int(row["workers"]), int(row["connections"]))].append(row)

def median(rows, field):
    return statistics.median(float(r[field]) for r in rows)


workers = sorted({w for w, _ in runs})
conns = sorted({c for _, c in runs})

for w in workers:
    print(f"\n### {w} worker{'s' if w != 1 else ''}\n")
    print("| connections | req/s | p50 | p95 | p99 | p99.9 | max | CPU (cores) | peak RSS |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for c in conns:
        rows = runs.get((w, c))
        if not rows:
            continue
        errors = sum(int(r["errors"]) for r in rows)
        # cpu_s covers the measured window, so dividing gives cores held busy
        cores = median(rows, "cpu_s") / duration
        note = "" if errors == 0 else f" ({errors} errors)"
        print(
            f"| {c} | {median(rows, 'rps'):,.0f}{note} "
            f"| {median(rows, 'p50_us')/1000:.2f} ms "
            f"| {median(rows, 'p95_us')/1000:.2f} ms "
            f"| {median(rows, 'p99_us')/1000:.2f} ms "
            f"| {median(rows, 'p999_us')/1000:.2f} ms "
            f"| {median(rows, 'max_us')/1000:.2f} ms "
            f"| {cores:.2f} "
            f"| {median(rows, 'peak_rss_kb')/1024:.1f} MB |"
        )

if len(workers) > 1:
    lo, hi = workers[0], workers[-1]
    print(f"\n### {hi} workers vs {lo}\n")
    print(f"| connections | {lo} worker | {hi} workers | speedup |")
    print("|---:|---:|---:|---:|")
    for c in conns:
        a, b = runs.get((lo, c)), runs.get((hi, c))
        if not a or not b:
            continue
        ra, rb = median(a, "rps"), median(b, "rps")
        print(f"| {c} | {ra:,.0f} | {rb:,.0f} | {rb/ra:.2f}x |")

spread = []
for key, rows in runs.items():
    values = [float(r["rps"]) for r in rows]
    if len(values) > 1 and statistics.median(values):
        spread.append((max(values) - min(values)) / statistics.median(values))
if spread:
    print(f"\nRun-to-run spread: max {max(spread)*100:.1f}%, median {statistics.median(spread)*100:.1f}%")
