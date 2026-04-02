#!/usr/bin/env python3
"""
SENTRY-RF Field Test Analyzer
Reads JSONL log files from the SD card and computes detection metrics.

Usage:
    python analyze_field_test.py field_0001.jsonl [--baseline-end 90]
    python analyze_field_test.py field_0001.jsonl --jammer-start 60 --jammer-stop 120
"""

import json
import sys
import argparse
from pathlib import Path


def load_jsonl(path):
    records = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return records


def compute_metrics(records, start_ms=None, end_ms=None):
    """Compute metrics over a time window."""
    filtered = records
    if start_ms is not None:
        filtered = [r for r in filtered if r["t"] >= start_ms]
    if end_ms is not None:
        filtered = [r for r in filtered if r["t"] <= end_ms]

    if not filtered:
        return None

    threats = [r["threat"] for r in filtered]
    divs = [r["div"] for r in filtered]
    confs = [r["conf"] for r in filtered]

    max_threat = max(threats)
    max_div = max(divs)
    avg_div = sum(divs) / len(divs)

    # Time to first WARNING (threat >= 2)
    t0 = filtered[0]["t"]
    first_warning = None
    first_critical = None
    for r in filtered:
        if r["threat"] >= 2 and first_warning is None:
            first_warning = (r["t"] - t0) / 1000.0
        if r["threat"] >= 3 and first_critical is None:
            first_critical = (r["t"] - t0) / 1000.0

    # Cycles at each level
    clear = sum(1 for t in threats if t == 0)
    advisory = sum(1 for t in threats if t == 1)
    warning = sum(1 for t in threats if t == 2)
    critical = sum(1 for t in threats if t == 3)
    total = len(threats)

    return {
        "cycles": total,
        "duration_s": (filtered[-1]["t"] - filtered[0]["t"]) / 1000.0,
        "max_threat": max_threat,
        "max_diversity": max_div,
        "avg_diversity": avg_div,
        "first_warning_s": first_warning,
        "first_critical_s": first_critical,
        "pct_clear": 100 * clear / total,
        "pct_advisory": 100 * advisory / total,
        "pct_warning": 100 * warning / total,
        "pct_critical": 100 * critical / total,
        "confirmed_cycles": sum(1 for c in confs if c > 0),
    }


def print_metrics(label, m):
    if m is None:
        print(f"\n=== {label} === (no data)")
        return
    print(f"\n=== {label} ===")
    print(f"  Duration:      {m['duration_s']:.1f}s ({m['cycles']} cycles)")
    print(f"  Max threat:    {m['max_threat']} ({'CLEAR ADVISORY WARNING CRITICAL'.split()[m['max_threat']]})")
    print(f"  Max diversity: {m['max_diversity']}")
    print(f"  Avg diversity: {m['avg_diversity']:.1f}")
    print(f"  First WARNING: {m['first_warning_s']:.1f}s" if m['first_warning_s'] else "  First WARNING: never")
    print(f"  First CRITICAL:{m['first_critical_s']:.1f}s" if m['first_critical_s'] else "  First CRITICAL:never")
    print(f"  CLEAR:    {m['pct_clear']:5.1f}%")
    print(f"  ADVISORY: {m['pct_advisory']:5.1f}%")
    print(f"  WARNING:  {m['pct_warning']:5.1f}%")
    print(f"  CRITICAL: {m['pct_critical']:5.1f}%")
    print(f"  Confirmed CAD cycles: {m['confirmed_cycles']}")


def main():
    parser = argparse.ArgumentParser(description="SENTRY-RF Field Test Analyzer")
    parser.add_argument("file", help="JSONL log file from SD card")
    parser.add_argument("--baseline-end", type=float, default=None,
                        help="Seconds into recording where baseline ends (no TX)")
    parser.add_argument("--jammer-start", type=float, default=None,
                        help="Seconds into recording where JAMMER starts")
    parser.add_argument("--jammer-stop", type=float, default=None,
                        help="Seconds into recording where JAMMER stops")
    parser.add_argument("--warmup", type=float, default=55,
                        help="Seconds to skip at start for warmup (default 55)")
    args = parser.parse_args()

    records = load_jsonl(args.file)
    if not records:
        print(f"No records in {args.file}")
        return

    t0 = records[0]["t"]
    warmup_ms = t0 + args.warmup * 1000

    print(f"File: {args.file}")
    print(f"Records: {len(records)}")
    print(f"Duration: {(records[-1]['t'] - t0) / 1000:.1f}s")
    print(f"Warmup skip: {args.warmup}s")

    # Full recording (post-warmup)
    post_warmup = [r for r in records if r["t"] >= warmup_ms]
    m_full = compute_metrics(post_warmup)
    print_metrics("FULL (post-warmup)", m_full)

    # Baseline segment
    if args.baseline_end:
        baseline_end_ms = t0 + args.baseline_end * 1000
        m_base = compute_metrics(post_warmup, end_ms=baseline_end_ms)
        print_metrics("BASELINE", m_base)

    # JAMMER segment
    if args.jammer_start:
        js_ms = t0 + args.jammer_start * 1000
        je_ms = t0 + args.jammer_stop * 1000 if args.jammer_stop else None
        m_jam = compute_metrics(records, start_ms=js_ms, end_ms=je_ms)
        print_metrics("JAMMER ACTIVE", m_jam)

    # Post-JAMMER (rapid-clear test)
    if args.jammer_stop:
        post_ms = t0 + args.jammer_stop * 1000
        m_post = compute_metrics(records, start_ms=post_ms)
        print_metrics("POST-JAMMER (rapid-clear)", m_post)

    # Diversity histogram
    print("\n=== DIVERSITY HISTOGRAM (post-warmup) ===")
    divs = [r["div"] for r in post_warmup]
    for d in range(max(divs) + 1):
        count = sum(1 for v in divs if v == d)
        bar = "#" * (count * 40 // len(divs)) if divs else ""
        print(f"  div={d:2d}: {count:4d} ({100*count/len(divs):5.1f}%) {bar}")


if __name__ == "__main__":
    main()
