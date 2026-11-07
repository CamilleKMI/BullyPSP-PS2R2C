#!/usr/bin/env python3
"""vu1batch.py batch_NNN - prints a captured VU1 batch: constants, the input records at TOP, the output GIF packets."""
import struct, sys
b = sys.argv[1]
d = open(b + ".in", "rb").read()
mem, rest = d[:16384], d[16384:]
regs = struct.unpack("<16I", rest[:64]); vf = [struct.unpack("<4f", rest[64 + 16*i:80 + 16*i]) for i in range(32)]
vi = struct.unpack("<16I", rest[64 + 512:64 + 512 + 64])
top, itop = regs[0], regs[1]
def q(a): return mem[16*a:16*a+16]
def qi(a): return struct.unpack("<4i", q(a))
def qf(a): return struct.unpack("<4f", q(a))
print("TOP %03x ITOP %d tops %03x base %03x ofst %03x" % (top, itop, regs[2], regs[3], regs[4]))
for a in (973, 974, 976, 1008, 1009, 1010, 1011, 1014, 1015, 1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023):
    print("  %4d: f %-50s i %s" % (a, " ".join("%.4g" % x for x in qf(a)), " ".join("%08x" % (x & 0xffffffff) for x in qi(a))))
n = min(itop, 6)
for k in range(n):
    print("  v%d:" % k, " | ".join(" ".join("%6d" % x for x in qi(top + 4*k + j)) for j in range(4)))
try:
    o = open(b + ".out", "rb").read()
except OSError:
    o = b""
i = 0; pk = 0
while i + 16 <= len(o) and pk < 2:
    lo, hi = struct.unpack("<QQ", o[i:i+16])
    nloop = lo & 0x7fff; eop = (lo >> 15) & 1; pre = (lo >> 46) & 1; prim = (lo >> 47) & 0x7ff; flg = (lo >> 58) & 3; nreg = (lo >> 60) & 15
    print("GIF tag nloop %d eop %d pre %d prim %03x flg %d nreg %d regs %016x" % (nloop, eop, pre, prim, flg, nreg, hi))
    i += 16
    for k in range(min(nloop, 4)):
        row = []
        for r in range(nreg):
            a, bb = struct.unpack("<QQ", o[i:i+16]); i += 16
            row.append("%016x:%016x" % (bb, a))
        print("   ", " ".join(row))
    i += 16 * nreg * max(0, nloop - 4)
    pk += 1
