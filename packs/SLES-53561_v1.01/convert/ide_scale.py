"""Stage transform (pack.toml [[stage.transform]]): the per-object draw distances of the binary object definitions
(.idb files in OBJECTS/IDE.IMG, listed by OBJECTS/IDE.DIR) multiplied by `factor`, only those under `below` (the
detailed models; the far LOD models keep theirs). Same file size (the disc map stays valid).

Format, per .idb: sections [u32 size][4cc 'sjbo' = 'objs' reversed][u32 count][u32 0], then count entries: u32 id,
model name and txd name (zero-terminated, padded to 4 bytes), u32 n (1..4), n floats (draw distances), 36 bytes of
flags / extras. Other sections are left as they are. (tools/ide_scale.py: the same, as a command.)"""
import struct


def _cstr_end(b, o):
    e = b.index(0, o)
    return (e + 1 + 3) & ~3


def scale(data, read, factor=0.75, below=200.0, dir="OBJECTS/IDE.DIR"):
    d = read(dir)
    img = bytearray(data)
    n_scaled = 0
    for i in range(len(d) // 32):
        so, sn = struct.unpack_from("<II", d, i * 32)
        off, end = so * 2048, (so + sn) * 2048
        while off + 16 <= end:
            size, tag, cnt, _z = struct.unpack_from("<I4sII", img, off)
            if size == 0 or size == 0xFFFFFFFF or off + size > end:
                break
            if tag == b"sjbo":
                p = off + 16
                for _k in range(cnt):
                    p2 = _cstr_end(img, p + 4)
                    p3 = _cstr_end(img, p2)
                    n = struct.unpack_from("<I", img, p3)[0]
                    if not 1 <= n <= 4:
                        raise ValueError("IDE.IMG: unexpected entry (%d distances) - another disc version?" % n)
                    for j in range(n):
                        a = p3 + 4 + 4 * j
                        v = struct.unpack_from("<f", img, a)[0]
                        if v < below:
                            struct.pack_into("<f", img, a, v * factor)
                            n_scaled += 1
                    p = p3 + 4 + 4 * n + 36
            off += size
    if not n_scaled:
        raise ValueError("IDE.IMG: no draw distance found - another disc version?")
    return bytes(img)
