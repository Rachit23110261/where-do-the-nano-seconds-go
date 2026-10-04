#!/usr/bin/env python3
import gzip, struct, sys
from collections import Counter

def ts(b):  # 6-byte big-endian nanoseconds since midnight
    ns = int.from_bytes(b, "big")
    s = ns // 1_000_000_000
    return f"{s//3600:02}:{s%3600//60:02}:{s%60:02}.{ns%1_000_000_000:09}"

def decode(m):
    t = chr(m[0])
    # Common header: type(1) locate(2) tracking(2) timestamp(6) = 11 bytes
    hdr = ts(m[5:11])
    if t == 'S':
        return f"{hdr} SYSTEM   event={chr(m[11])}"
    if t == 'R':
        return f"{hdr} STOCKDIR {m[11:19].decode().strip()}"
    if t in 'AF':
        ref, side, sh, stk, px = struct.unpack(">QcI8sI", m[11:36])
        return f"{hdr} ADD      ref={ref} {side.decode()} {sh} {stk.decode().strip()} @ {px/1e4:.4f}"
    if t == 'E':
        ref, sh, match = struct.unpack(">QIQ", m[11:31])
        return f"{hdr} EXEC     ref={ref} shares={sh}"
    if t == 'C':
        ref, sh, match, pr, px = struct.unpack(">QIQcI", m[11:36])
        return f"{hdr} EXEC_PX  ref={ref} shares={sh} @ {px/1e4:.4f}"
    if t == 'X':
        ref, sh = struct.unpack(">QI", m[11:23])
        return f"{hdr} CANCEL   ref={ref} shares={sh}"
    if t == 'D':
        (ref,) = struct.unpack(">Q", m[11:19])
        return f"{hdr} DELETE   ref={ref}"
    if t == 'U':
        old, new, sh, px = struct.unpack(">QQII", m[11:35])
        return f"{hdr} REPLACE  {old}->{new} {sh} @ {px/1e4:.4f}"
    return None  # other types: just counted

path, limit = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 50
counts, printed = Counter(), 0
with gzip.open(path, "rb") as f:
    while True:
        lb = f.read(2)
        if len(lb) < 2: break
        (n,) = struct.unpack(">H", lb)
        m = f.read(n)
        if len(m) < n: break          # truncated tail (partial download)
        counts[chr(m[0])] += 1
        if printed < limit:
            s = decode(m)
            if s: print(s); printed += 1
        elif sum(counts.values()) >= 5_000_000:
            break                     # stop early; remove to scan whole file
print("\nMessage type counts:", dict(counts.most_common()))
