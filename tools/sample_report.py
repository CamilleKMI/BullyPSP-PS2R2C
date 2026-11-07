#!/usr/bin/env python3
"""sample_report.py BINARY SAMPLES [N] - folds hle_sampler.c's program-counter samples onto the host binary's
functions (nm -n) and prints the top N (default 40) with their share; translated game functions are fn_XXXXXXXX."""
import subprocess, sys, bisect
binary, samples = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
syms = []
for line in subprocess.run(["nm", "-n", "--defined-only", binary], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) >= 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
# PIE: find the load bias from the main symbol when needed
pcs = [(int(a, 16), int(c)) for a, c in (l.split() for l in open(samples))]
bias = 0
if pcs and min(p for p, _ in pcs) > 0x400000000000 and addrs and addrs[-1] < 0x100000000:
    # guess the bias as the page-aligned difference that puts most samples inside the text range
    lo = min(p for p, _ in pcs)
    bias = (lo - addrs[0]) & ~0xFFF
acc = {}
total = 0
for pc, c in pcs:
    i = bisect.bisect_right(addrs, pc - bias) - 1
    name = syms[i][1] if i >= 0 else "?"
    acc[name] = acc.get(name, 0) + c
    total += c
groups = {"translated game code (fn_*)": 0, "runtime / libc": 0}
for n, c in acc.items():
    groups["translated game code (fn_*)" if n.startswith("fn_") else "runtime / libc"] += c
print("samples %d" % total)
for g, c in groups.items():
    print("  %-30s %5.1f%%" % (g, 100.0 * c / max(1, total)))
for n, c in sorted(acc.items(), key=lambda x: -x[1])[:top]:
    print("%6.2f%%  %s" % (100.0 * c / max(1, total), n))
