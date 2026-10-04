#!/usr/bin/env python3
"""analyze_trace.py - percentile report for itch_rev3 flight-recorder traces.

Usage: python3 analyze_trace.py trace.bin [--skip 0.05] [--stall-us 10]

  --skip      fraction of records to drop at the start (warm-up), default 0.05
  --stall-us  queue-wait threshold reported as a "stall", default 10 us
"""
import argparse
import sys

import numpy as np

HDR = np.dtype([("magic", "S8"), ("cpn", "<f8"), ("records", "<u8"), ("dropped", "<u8")])
REC = np.dtype([("t_rx", "<u8"), ("t_deq", "<u8"), ("t_book", "<u8"),
                ("type", "u1"), ("pad", "V7")])
PCTS = [50, 90, 99, 99.9, 99.99]


def row(name, ns):
    if ns.size == 0:
        return f"{name:<22}{'(no data)':>10}"
    p = np.percentile(ns, PCTS)
    return (f"{name:<22}" + "".join(f"{v:>10.0f}" for v in p)
            + f"{ns.max():>12.0f}{ns.mean():>10.0f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--skip", type=float, default=0.05)
    ap.add_argument("--stall-us", type=float, default=10.0)
    a = ap.parse_args()

    h = np.fromfile(a.trace, dtype=HDR, count=1)[0]
    if h["magic"] != b"ITCHTRC1":
        sys.exit("not an ITCHTRC1 trace file")
    cpn = float(h["cpn"])
    r = np.fromfile(a.trace, dtype=REC, offset=HDR.itemsize)
    print(f"file      : {a.trace}")
    print(f"records   : {r.size:,} (header says {int(h['records']):,}), "
          f"dropped in recorder: {int(h['dropped']):,}")
    print(f"TSC       : {cpn:.4f} cycles/ns")

    skip = int(r.size * a.skip)
    r = r[skip:]
    print(f"warm-up   : skipped first {skip:,} records ({a.skip:.0%})\n")

    t_rx = r["t_rx"].astype(np.int64)
    t_deq = r["t_deq"].astype(np.int64)
    t_book = r["t_book"].astype(np.int64)
    queue = (t_deq - t_rx) / cpn
    book = (t_book - t_deq) / cpn
    total = (t_book - t_rx) / cpn

    neg = int((queue < 0).sum())
    if neg:
        print(f"WARNING: {neg:,} records with t_deq < t_rx -> TSC not synchronised "
              "across cores (common on VMs). Treat cross-core 'queue' numbers with care.\n")

    hdr = f"{'stage (ns)':<22}" + "".join(f"{'p'+str(p):>10}" for p in PCTS) \
        + f"{'max':>12}{'mean':>10}"
    print(hdr)
    print("-" * len(hdr))
    print(row("queue  (rx -> deq)", queue))
    print(row("book   (deq -> book)", book))
    print(row("total  (rx -> book)", total))

    # Book cost by message type (same-core delta: always trustworthy)
    print(f"\n{'book cost by type':<22}{'count':>12}{'p50':>10}{'p99':>10}{'p99.9':>10}{'max':>12}")
    types = r["type"]
    for t in sorted(set(types.tolist()), key=lambda x: -int((types == x).sum())):
        b = book[types == t]
        if b.size < 100:
            continue
        p = np.percentile(b, [50, 99, 99.9])
        print(f"  {chr(t):<20}{b.size:>12,}{p[0]:>10.0f}{p[1]:>10.0f}{p[2]:>10.0f}{b.max():>12.0f}")

    # Stalls: messages that waited a long time in the queue
    thr = a.stall_us * 1000
    stall = queue > thr
    print(f"\nqueue wait > {a.stall_us:g} us : {int(stall.sum()):,} records "
          f"({100 * stall.mean():.4f}%)")
    # Group consecutive stalled records into episodes
    if stall.any():
        idx = np.flatnonzero(stall)
        breaks = np.flatnonzero(np.diff(idx) > 1)
        starts = np.r_[idx[0], idx[breaks + 1]]
        ends = np.r_[idx[breaks], idx[-1]]
        worst = sorted(zip(starts, ends), key=lambda se: -queue[se[0]:se[1] + 1].max())[:10]
        print(f"stall episodes : {len(starts):,}  (worst 10 by peak queue wait)")
        for s, e in worst:
            print(f"  records {s:>10,}-{e:<10,} len={e - s + 1:>6,}  "
                  f"peak={queue[s:e + 1].max() / 1000:>9.1f} us")


if __name__ == "__main__":
    main()

