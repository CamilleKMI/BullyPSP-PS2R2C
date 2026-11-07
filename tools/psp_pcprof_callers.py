#!/usr/bin/env python3
"""psp_pcprof_callers.py ELF PCPROF FUNC... - the samples inside each function by caller (the sampled ra)."""
import subprocess, bisect, collections, sys
elf, f = sys.argv[1], sys.argv[2]
syms = []
for l in subprocess.run(["psp-nm", "-n", "-S", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) >= 4 and p[2] in "tTwW": syms.append((int(p[0], 16), int(p[1], 16), p[3]))
addrs = [a for a, _, _ in syms]
def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    return syms[i][2] if i >= 0 and a - addrs[i] < syms[i][1] + 4 else "?"
rows = [(int(a, 16), int(b, 16), int(c)) for a, b, c in (l.split() for l in open(f))]
for fn in sys.argv[3:]:
    lo = [s for s in syms if s[2] == fn]
    if not lo: print("no", fn); continue
    lo = lo[0]; c = collections.Counter(); n = 0
    for pc, ra, k in rows:
        if lo[0] <= pc < lo[0] + lo[1]: c[name(ra)] += k; n += k
    print(fn + ":", ", ".join("%s %.0f%%" % (k, 100.0 * v / n) for k, v in c.most_common(5)))
