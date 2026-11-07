#!/usr/bin/env python3
"""psp_pcprof_lines.py ELF PCPROF FUNC [N] - the samples of one function by source line (psp-addr2line) and its
hottest instructions (psp-objdump), from PPSSPP_PCPROF output."""
import subprocess, collections, sys
elf, f, fn = sys.argv[1], sys.argv[2], sys.argv[3]; top = int(sys.argv[4]) if len(sys.argv) > 4 else 25
lo = hi = None
for l in subprocess.run(["psp-nm", "-n", "-S", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) >= 4 and p[3] == fn: lo = int(p[0], 16); hi = lo + int(p[1], 16)
if lo is None: sys.exit("no symbol " + fn)
c = collections.Counter(); n = 0
for l in open(f):
    pc, ra, k = l.split(); pc = int(pc, 16); k = int(k)
    if pc != 0x08000000: n += k
    if lo <= pc < hi: c[pc] += k
tot = sum(c.values())
pcs = sorted(c)
lines = subprocess.run(["psp-addr2line", "-e", elf] + ["%x" % p for p in pcs], capture_output=True, text=True).stdout.splitlines()
lines = [x.split(" (discriminator")[0] for x in lines]
bl = collections.Counter()
for p, ln in zip(pcs, lines): bl[ln.split("/")[-1]] += c[p]
print("%s: %.2f%% of the busy CPU" % (fn, 100.0 * tot / n))
for ln, v in bl.most_common(top): print("%6.1f%%  %s" % (100.0 * v / tot, ln))
dis = {}
for l in subprocess.run(["psp-objdump", "-d", "--start-address=%#x" % lo, "--stop-address=%#x" % hi, elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split(":", 1)
    try: dis[int(p[0].strip(), 16)] = p[1].strip()
    except (ValueError, IndexError): pass
print("hottest instructions:")
for p, v in c.most_common(top): print("%6.1f%%  %08x  %s" % (100.0 * v / tot, p, dis.get(p, "")[:70]))
