#!/usr/bin/env python3
"""ide_scale.py IDE.DIR IDE.IMG OUT.IMG FACTOR [--below D] [--dump] - the binary object definitions (.idb in OBJECTS/IDE.IMG):
multiplies every object's draw distance(s) by FACTOR (same file size: the disc layout stays). Format, per file:
sections [u32 size][4cc reversed 'objs'][u32 count][u32 0] then count entries: u32 id, model name and txd name
(zero-terminated, padded to 4), u32 n, n floats (draw distances), 36 bytes of flags / extras. Other sections are
left as they are. --below D: only distances under D (the detailed models; the far LOD models keep theirs). The output is made from the user's own disc: keep it private."""
import struct, sys
dirf, imgf, outf, fac = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
dump = '--dump' in sys.argv
below = float(sys.argv[sys.argv.index('--below') + 1]) if '--below' in sys.argv else 1e30
d = open(dirf, 'rb').read(); img = bytearray(open(imgf, 'rb').read())
def cstr(b, o):
    e = b.index(0, o); s = b[o:e]; return s, (e + 1 + 3) & ~3
tot = 0; hist = {}
for i in range(len(d) // 32):
    so, sn, name = struct.unpack_from('<II24s', d, i * 32)
    name = name.split(b'\0')[0].decode()
    base, end = so * 2048, (so + sn) * 2048
    off = base
    while off + 16 <= end:
        size, tag, cnt, z = struct.unpack_from('<I4sII', img, off)
        if size == 0 or size == 0xFFFFFFFF or off + size > end: break
        if tag == b'sjbo':
            p = off + 16
            for k in range(cnt):
                oid = struct.unpack_from('<I', img, p)[0]
                mdl, p2 = cstr(img, p + 4); txd, p3 = cstr(img, p2)
                n = struct.unpack_from('<I', img, p3)[0]
                assert 1 <= n <= 4, (name, k, n)
                for j in range(n):
                    a = p3 + 4 + 4 * j
                    v = struct.unpack_from('<f', img, a)[0]
                    hist[int(v)] = hist.get(int(v), 0) + 1
                    if dump and j == 0: print(name, oid, mdl.decode(errors='replace'), v)
                    if v < below: struct.pack_into('<f', img, a, v * fac); tot += 1
                p = p3 + 4 + 4 * n + 36
            assert p <= off + size, (name, hex(p), hex(off + size))
        off += size
open(outf, 'wb').write(img)
print('%d draw distances scaled by %g' % (tot, fac), file=sys.stderr)
top = sorted(hist.items(), key=lambda x: -x[1])[:15]
print('most common distances:', top, file=sys.stderr)
