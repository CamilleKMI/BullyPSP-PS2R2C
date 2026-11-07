"""fnstrings.py ELF FUNCTIONS.pkl ADDR... - short printable strings a function builds addresses of (lui+addiu)."""
import os, pickle, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "PS2RC"))
from ps2rc.image import load_elf
from ps2rc.r5900 import decode
img = load_elf(sys.argv[1]); f = pickle.load(open(sys.argv[2], "rb"))
def cstr(a):
    s = b""
    try:
        while len(s) < 60:
            b = img.word(a & ~3) >> (8 * (a & 3)) & 0xFF
            if b == 0: break
            if b < 32 or b > 126: return None
            s += bytes([b]); a += 1
    except Exception:
        return None
    return s.decode() if len(s) >= 3 else None
for t in sys.argv[3:]:
    fn = f[int(t, 16)]; regs = {}; out = []
    for pc in sorted(fn.addrs):
        i = decode(img.word(pc), pc)
        if i.mnem == "lui": regs[i.ops[0][1]] = i.ops[1][1] << 16
        elif i.mnem == "addiu" and i.ops[1][1] in regs:
            s = cstr((regs[i.ops[1][1]] + i.ops[2][1]) & 0xFFFFFFFF)
            if s: out.append(s)
        elif i.mnem == "addiu" and i.ops[1][1] == 28:
            s = cstr((img.gp + i.ops[2][1]) & 0xFFFFFFFF)
            if s: out.append(s)
    print(t, out[:12])
