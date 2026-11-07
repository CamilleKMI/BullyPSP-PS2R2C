#!/usr/bin/env python3
"""psp_pcprof_report.py ELF PCPROF [N] - PPSSPP_PCPROF output ("pc ra n") folded onto the ELF's functions: share of
the busy CPU (idle pc 0x08000000 left out) per function, translated game code (fn_*) in total."""
import subprocess, bisect, collections, sys
elf, f = sys.argv[1], sys.argv[2]; top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
syms = []
for l in subprocess.run(["psp-nm", "-n", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) >= 3 and p[1] in "tTwW": syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    return syms[i][1] if i >= 0 and a - addrs[i] < 0x40000 else "?"
c = collections.Counter(); n = 0
for l in open(f):
    pc, ra, k = l.split(); pc = int(pc, 16); k = int(k)
    if pc == 0x08000000: continue
    n += k; c[name(pc)] += k
g = sum(v for k, v in c.items() if k.startswith("fn_"))
print("busy samples %d; translated game code %.1f%%" % (n, 100.0 * g / n))
for k, v in c.most_common(top): print("%6.2f%%  %s" % (100.0 * v / n, k))
