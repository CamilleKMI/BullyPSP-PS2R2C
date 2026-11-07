"""constfind.py ELF FUNCTIONS.pkl CONST... - functions that build a 32-bit constant with lui + ori/addiu (game-derived
output: local only)."""
import bisect, os, pickle, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "PS2RC"))
from ps2rc.image import load_elf
from ps2rc.r5900 import decode
img = load_elf(sys.argv[1]); f = pickle.load(open(sys.argv[2], "rb")); starts = sorted(f)
want = {int(x, 16): x for x in sys.argv[3:]}
lo, hi = 0x100000, 0x53f300
hits = {}
for a in range(lo, hi, 4):
    i = decode(img.word(a), a)
    if i.mnem != "lui":
        continue
    r, hv = i.ops[0][1], i.ops[1][1]
    for b in range(a + 4, min(a + 40, hi), 4):
        j = decode(img.word(b), b)
        if j.mnem in ("addiu", "ori") and j.ops[1][1] == r:
            v = (((hv << 16) + j.ops[2][1]) if j.mnem == "addiu" else ((hv << 16) | j.ops[2][1])) & 0xFFFFFFFF
            if v in want:
                k = bisect.bisect_right(starts, b) - 1
                hits.setdefault(want[v], set()).add(starts[k])
            break
for k, v in hits.items():
    print(k, " ".join("%08x" % x for x in sorted(v)))
