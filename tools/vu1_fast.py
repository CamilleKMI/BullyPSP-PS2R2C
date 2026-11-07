#!/usr/bin/env python3
"""vu1_fast.py CODE PCHIST OUT.h [LO:HI:HASH ...] - a VU1 program translated to C without the per-instruction timing model.

Same values as the exact model (hle_vu1.c) when the program does not depend on the cycle counts across basic
blocks: inside a block the issue cycles (stalls on VF results 4 cycles away, Q 7 / 13) are computed here, once;
at run time a block only adds its length. The flag / Q queues are the model's own (vu.macq / clq / q_*), fed with
those cycles: MAC pushes only for the FMACs whose flags a FMAND / FMOR / FMEQ may read (no status reads in the code).
Operands are not clamped (results are). Instructions not in PCHIST hand back to the interpreter (return pc).
Check on captured batches with tools/vu1lab. Output is game-derived: keep private."""
import struct, sys

code = open(sys.argv[1], "rb").read()
hist = {int(a, 16) for a, _ in (l.split() for l in open(sys.argv[2]))}
out = open(sys.argv[3], "w")
progs = [tuple(int(x, 16) for x in p.split(":")) for p in sys.argv[4:]] or [(0, 0x555, 0xc3651f62), (0x555, 0x800, 0x3d8ad419)]

def word(pc, k): return struct.unpack_from("<I", code, (pc & 0x7FF) * 8 + 4 * k)[0]
LN = "xyzw"
def lanes(m): return [i for i in range(4) if m & (8 >> i)]

# ---------------------------------------------------------------- decode
def dec_upper(w):
    dest, ft, fs, fd, f = (w >> 21) & 15, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    d = dict(dest=dest, ft=ft, fs=fs, fd=fd, kind=None, bc=-1, src2=0, toacc=0, out=fd)
    if f < 28: d.update(kind="ar", base=f >> 2, bc=f & 3); return d
    if f < 60:
        t = {28: (6, 2), 29: (4, 1), 30: (6, 1), 31: (5, 1), 32: (0, 2), 33: (2, 2), 34: (0, 1), 35: (2, 1), 36: (1, 2),
             37: (3, 2), 38: (1, 1), 39: (3, 1), 40: (0, 0), 41: (2, 0), 42: (6, 0), 43: (4, 0), 44: (1, 0), 45: (3, 0), 47: (5, 0)}
        if f in t: d.update(kind="ar", base=t[f][0], src2=t[f][1]); return d
        return None
    c = ((w >> 6) & 31) << 2 | (w & 3)
    d["toacc"] = 1
    if c < 28 and (c >> 2) not in (4, 5):
        d.update(kind="ar", base=[0, 1, 2, 3, -1, -1, 6][c >> 2], bc=c & 3); return d
    if 16 <= c <= 19: d.update(kind="itof", sc=c - 16, toacc=0); return d
    if 20 <= c <= 23: d.update(kind="ftoi", sc=c - 20, toacc=0); return d
    if c == 29: d.update(kind="abs", toacc=0); return d
    if c == 31: d.update(kind="clip", toacc=0); return d
    if c == 47: d.update(kind="nop", toacc=0); return d
    t = {28: (6, 2), 30: (6, 1), 32: (0, 2), 33: (2, 2), 34: (0, 1), 35: (2, 1), 36: (1, 2), 37: (3, 2), 38: (1, 1), 39: (3, 1),
         40: (0, 0), 41: (2, 0), 42: (6, 0), 44: (1, 0), 45: (3, 0)}
    if c in t: d.update(kind="ar", base=t[c][0], src2=t[c][1]); return d
    return None

def dec_lower(w, pc):
    dest, ft, fs, idd = (w >> 21) & 15, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    d = dict(dest=dest, ft=ft, fs=fs, it=ft & 15, is_=fs & 15, id=idd & 15, fsf=(w >> 21) & 3, ftf=(w >> 23) & 3, w=w)
    imm11 = w & 0x7FF
    if imm11 & 0x400: imm11 -= 0x800
    d["imm11"] = imm11; d["tgt"] = (pc + 1 + imm11) & 0x7FF
    if w >> 31:
        if (w >> 25) != 0x40: return None
        f = w & 63
        if f < 60:
            m = {48: "iadd", 49: "isub", 50: "iaddi", 52: "iand", 53: "ior"}
            d["op"] = m.get(f); return d if d["op"] else None
        c = ((w >> 6) & 31) << 2 | (w & 3)
        m = {48: "move", 49: "mr32", 52: "lqi", 53: "sqi", 54: "lqd", 55: "sqd", 56: "div", 57: "sqrt", 58: "rsqrt", 59: "waitq",
             60: "mtir", 61: "mfir", 62: "ilwr", 63: "iswr", 104: "xtop", 105: "xitop", 108: "xgkick"}
        d["op"] = m.get(c); return d if d["op"] else None
    op = w >> 25
    m = {0x00: "lq", 0x01: "sq", 0x04: "ilw", 0x05: "isw", 0x08: "iaddiu", 0x09: "isubiu", 0x12: "fcand", 0x13: "fcor",
         0x1A: "fmand", 0x1B: "fmor", 0x18: "fmeq", 0x1C: "fcget", 0x11: "fcset", 0x20: "b", 0x24: "jr", 0x28: "ibeq",
         0x29: "ibne", 0x2C: "ibltz", 0x2D: "ibgtz", 0x2E: "iblez", 0x2F: "ibgez", 0x10: "fceq"}
    d["op"] = m.get(op)
    return d if d["op"] else None

BR = {"b", "jr", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"}
INS = {}
for pc in sorted(hist):
    lw, uw = word(pc, 0), word(pc, 1)
    u = dec_upper(uw)
    l = None if uw >> 31 else dec_lower(lw, pc)
    if u is None or (not (uw >> 31) and l is None):
        sys.exit("unsupported instruction at %03x: %08x %08x" % (pc, lw, uw))
    INS[pc] = dict(u=u, l=l, loi=bool(uw >> 31), lw=lw, uw=uw, e=bool((uw >> 30) & 1))

def is_br(pc): return pc in INS and INS[pc]["l"] and INS[pc]["l"]["op"] in BR

# jr targets: the values the program puts in integer registers with IADDIU from vi00 (addresses it jumps to)
jr_t = set()
for pc, x in INS.items():
    l = x["l"]
    if l and l["op"] == "iaddiu" and l["is_"] == 0:
        v = ((l["w"] >> 10) & 0x7800) | (l["w"] & 0x7FF)
        if v in INS: jr_t.add(v)
entries = {p[0] for p in progs} | {0}
leaders = set(e for e in entries if e in INS) | jr_t
for pc in INS:
    l = INS[pc]["l"]
    if l and l["op"] in BR:
        if l["op"] != "jr" and l["tgt"] in INS: leaders.add(l["tgt"])
        if pc + 2 in INS: leaders.add(pc + 2)
    if INS[pc]["e"] and pc + 2 in INS: leaders.add(pc + 2)

# MAC readers and liveness (no status flag readers: checked)
def lower_reads_mac(pc): return pc in INS and INS[pc]["l"] and INS[pc]["l"]["op"] in ("fmand", "fmor", "fmeq")
assert not any(x["l"] and x["l"]["op"] in ("fsand", "fsor", "fseq") for x in INS.values())
def succs(pc):
    """(successor pcs) of the instruction at pc, delay slots included as plain successors."""
    l = INS[pc]["l"]
    if INS[pc]["e"]: return [pc + 1] if not (pc > 0 and INS.get(pc - 1, {}).get("e")) else []
    if pc - 1 in INS and is_br(pc - 1):                              # delay slot: where the branch goes
        b = INS[pc - 1]["l"]
        if b["op"] == "jr": return sorted(jr_t)
        r = [b["tgt"]]
        if b["op"] != "b": r.append(pc + 1)
        return r
    if pc - 1 in INS and INS[pc - 1]["e"]: return []
    return [pc + 1]
def mac_live(p0):
    seen = set(); st = [(s, 1) for s in succs(p0)]
    while st:
        pc, d = st.pop()
        if d > 12: continue
        if pc not in INS or (pc - 1 in INS and INS[pc - 1]["e"]): return True      # unknown code / the program end
        if (pc, d) in seen: continue
        seen.add((pc, d))
        if lower_reads_mac(pc): return True
        st += [(s, d + 1) for s in succs(pc)]
    return False
def sets_mac(u): return u["kind"] == "ar" and u["base"] not in (4, 5)
def mac_superseded(p0):
    """1: on every path from p0, a later FMAC sets MAC before any reader, and no reader comes within 3 instructions
    of it (issue order: a reader 4+ instructions after an FMAC issues 4+ cycles after it, when its flags are ready)."""
    seen = set(); st = [(s, -1) for s in succs(p0)]      # (pc, -1: before a newer FMAC / k: instructions since it)
    while st:
        pc, d = st.pop()
        if pc not in INS or (pc - 1 in INS and INS[pc - 1]["e"]): return False
        if (pc, d) in seen: continue
        seen.add((pc, d))
        if len(seen) > 400: return False
        if lower_reads_mac(pc) and d < 4: return False
        if d >= 4: continue
        nd = 0 if sets_mac(INS[pc]["u"]) else (d + 1 if d >= 0 else -1)
        if nd >= 4: continue
        nx = succs(pc)
        if not nx: return False
        st += [(s, nd) for s in nx]
    return True
live = {pc for pc, x in INS.items() if sets_mac(x["u"]) and mac_live(pc)}
NDEAD = [0]
for pc in sorted(live):
    if mac_superseded(pc): live.discard(pc); NDEAD[0] += 1

# predecessors of branches reading integer registers (the model's "old value after an integer op" rule)
brpred = set()
for pc in INS:
    if is_br(pc) and INS[pc]["l"]["op"] != "b":
        brpred.add(pc - 1)
        for q in INS:
            if is_br(q) and INS[q]["l"]["op"] not in ("jr",) and INS[q]["l"]["tgt"] == pc: brpred.add(q + 1)
        if pc in jr_t:
            for q in INS:
                if is_br(q) and INS[q]["l"]["op"] == "jr": brpred.add(q + 1)

# ---------------------------------------------------------------- per instruction C
# registers: float locals f<r>_<lane> (loaded at entry, written back at every exit), vf00 constant, ACC a<lane>_
CLAMP = int(__import__("os").environ.get("FAST_CLAMP", "0"))                                 # 1: results saturated as the model does (fres); 0: plain float results
USED = set()
def vf(r, i):
    if r == 0: return "1.0f" if i == 3 else "0.0f"
    USED.add(r); return "f%d_%d" % (r, i)
def vfu(r, i):
    if r == 0: return "0x3F800000u" if i == 3 else "0u"
    USED.add(r); return "asu_(f%d_%d)" % (r, i)
def vfi(r, i): return "(int32_t)%s" % vfu(r, i)
def wu(r, i, e):
    if r == 0: return ""
    USED.add(r); return "f%d_%d = asf_(%s);" % (r, i, e)
def wf(r, i, e):
    if r == 0: return ""
    USED.add(r); return "f%d_%d = %s;" % (r, i, e)

def upper_c(u, pc):
    """(pre: computing temporaries, commit: writing them, srcregs, dstreg)"""
    k = u["kind"]
    if k == "nop": return "", "", [], None
    L = lanes(u["dest"])
    fs, ft = u["fs"], u["ft"]
    if k == "clip":
        s = ["{ float lim_ = fabsf(%s); uint32_t c_ = 0;" % vf(ft, 3)]
        for i in range(3): s.append(" if (%s > lim_) c_ |= %du; if (%s < -lim_) c_ |= %du;" % (vf(fs, i), 1 << (2 * i), vf(fs, i), 2 << (2 * i)))
        s.append(" push_clip_t_(c_, T + %(C)d); }")
        return "".join(s), "", [fs, ft], None
    pre, com = [], []
    if k == "itof":
        sc = ["1.0f", "(1.0f / 16)", "(1.0f / 4096)", "(1.0f / 32768)"][u["sc"]]
        for i in L: pre.append("uint32_t t%d_ = fres((float)%s * %s);" % (i, vfi(fs, i), sc))
        dst = ft
        for i in L: com.append(wu(dst, i, "t%d_" % i))
        return " ".join(pre), " ".join(com) if dst else "", [fs], dst
    if k == "ftoi":
        sc = ["1.0f", "16.0f", "4096.0f", "32768.0f"][u["sc"]]
        for i in L: pre.append("int32_t t%d_ = ftoi_(%s * %s);" % (i, vf(fs, i), sc))
        dst = ft
        for i in L: com.append(wu(dst, i, "(uint32_t)t%d_" % i))
        return " ".join(pre), " ".join(com) if dst else "", [fs], dst
    if k == "abs":
        for i in L: pre.append("uint32_t t%d_ = %s & 0x7FFFFFFFu;" % (i, vfu(fs, i)))
        for i in L: com.append(wu(ft, i, "t%d_" % i))
        return " ".join(pre), " ".join(com) if ft else "", [fs], ft
    base, src2, bc = u["base"], u["src2"], u["bc"]
    srcs = [fs] + ([ft] if src2 == 0 else [])
    def B(i): return "vu.I" if src2 == 1 else "vu.Q" if src2 == 2 else vf(ft, bc if bc >= 0 else i)
    def BU(i): return "fres_u(vu.I)" if src2 == 1 else "fres_u(vu.Q)" if src2 == 2 else vfu(ft, bc if bc >= 0 else i)
    for i in L:
        a = vf(fs, i)
        if base == 0: e = "%s + %s" % (a, B(i))
        elif base == 1: e = "%s - %s" % (a, B(i))
        elif base == 2: e = "a%d_ + %s * %s" % (i, a, B(i))
        elif base == 3: e = "a%d_ - %s * %s" % (i, a, B(i))
        elif base == 6: e = "%s * %s" % (a, B(i))
        else:
            pre.append("float t%d_ = asf_(%s(%s, %s));" % (i, "smax_" if base == 4 else "smin_", vfu(fs, i), BU(i)))
            continue
        pre.append(("float t%d_ = asf_(fres(%s));" if CLAMP else "float t%d_ = %s;") % (i, e))   # (fresf_: slower in PPSSPP)
    if base not in (4, 5) and pc in live:
        m = []
        for i in L: m.append("(!(asu_(t%d_) & 0x7FFFFFFFu) ? %du : 0u) | ((asu_(t%d_) >> 31) ? %du : 0u)" % (i, 8 >> i, i, 0x80 >> i))
        pre.append("uint32_t mac_ = %s;" % (" | ".join(m) if m else "0"))
    dst = "acc" if u["toacc"] else u["out"]
    for i in L:
        com.append(("a%d_ = t%d_;" % (i, i)) if dst == "acc" else wf(dst, i, "t%d_" % i))
    if dst != "acc" and dst == 0: com = []
    if base not in (4, 5) and pc in live: com.append("push_mac_t_(mac_, T + %(C)d);")
    return " ".join(pre), " ".join(com), srcs, (None if dst in ("acc", 0) else dst)

USED_VI = set()
def vi(r):
    if r == 0: return "0"
    USED_VI.add(r); return "i%d_" % r
def setvi(r, e, pc):
    if r == 0: return "(void)(%s);" % e
    if pc in brpred: return "{ uint16_t o_ = %s; %s = (uint16_t)(%s); pvr = %d; pvo = o_; }" % (vi(r), vi(r), e, r)
    return "%s = (uint16_t)(%s);" % (vi(r), e)

def lower_c(l, pc):
    """(code, srcregs (stall), is_reader, branch: (cond C, target) or None)"""
    op = l["op"]; L = lanes(l["dest"]); ft, fs, it, is_, idd = l["ft"], l["fs"], l["it"], l["is_"], l["id"]
    pv = "pvr = -1;" if pc in brpred else ""
    def mem(a): return "((uint32_t *)vmem((uint32_t)(%s)))" % a
    if op in ("lq", "lqi", "lqd"):
        a = "%s + %d" % (vi(is_), l["imm11"]) if op == "lq" else vi(is_)
        s = []
        if op == "lqd": s.append(setvi(is_, "%s - 1" % vi(is_), pc)); a = vi(is_)
        if ft: s.append("{ const uint32_t *m_ = %s; %s }" % (mem(a), " ".join(wu(ft, i, "m_[%d]" % i) for i in L)))
        if op == "lqi": s.append(setvi(is_, "%s + 1" % vi(is_), pc))
        elif pv and op == "lq": s.append(pv)
        return " ".join(s), [], False, None
    if op in ("sq", "sqi", "sqd"):
        a = "%s + %d" % (vi(it), l["imm11"]) if op == "sq" else vi(it)
        s = []
        if op == "sqd": s.append(setvi(it, "%s - 1" % vi(it), pc)); a = vi(it)
        s.append("{ uint32_t *m_ = %s; %s }" % (mem(a), " ".join("m_[%d] = %s;" % (i, vfu(fs, i)) for i in L)))
        if op == "sqi": s.append(setvi(it, "%s + 1" % vi(it), pc))
        elif pv and op == "sq": s.append(pv)
        return " ".join(s), [fs], False, None
    if op == "ilw":
        i0 = L[0] if L else 0
        return setvi(it, "%s[%d] & 0xFFFF" % (mem("%s + %d" % (vi(is_), l["imm11"])), i0), pc), [], False, None
    if op == "ilwr":
        i0 = L[0] if L else 0
        return setvi(it, "%s[%d] & 0xFFFF" % (mem(vi(is_)), i0), pc), [], False, None
    if op == "isw":
        return "{ uint32_t *m_ = %s; %s } %s" % (mem("%s + %d" % (vi(is_), l["imm11"])), " ".join("m_[%d] = %s;" % (i, vi(it)) for i in L), pv), [], False, None
    if op == "iaddiu": return setvi(it, "%s + %d" % (vi(is_), ((l["w"] >> 10) & 0x7800) | (l["w"] & 0x7FF)), pc), [], False, None
    if op == "isubiu": return setvi(it, "%s - %d" % (vi(is_), ((l["w"] >> 10) & 0x7800) | (l["w"] & 0x7FF)), pc), [], False, None
    if op == "iaddi":
        v = (l["w"] >> 6) & 31
        if v & 16: v -= 32
        return setvi(it, "%s + %d" % (vi(is_), v), pc), [], False, None
    if op in ("iadd", "isub", "iand", "ior"):
        o = {"iadd": "+", "isub": "-", "iand": "&", "ior": "|"}[op]
        return setvi(idd, "%s %s %s" % (vi(is_), o, vi(it)), pc), [], False, None
    if op == "move":
        if not ft or not L: return pv, [], False, None
        return "%s %s" % (" ".join(wf(ft, i, vf(fs, i)) for i in L), pv), [fs], False, None
    if op == "mr32":
        if not ft: return pv, [fs], False, None
        return "{ float s_[4] = {%s}; %s } %s" % (", ".join(vf(fs, i) for i in range(4)), " ".join(wf(ft, i, "s_[%d]" % ((i + 1) & 3)) for i in L), pv), [fs], False, None
    if op == "mtir": return setvi(it, "%s & 0xFFFF" % vfu(fs, l["fsf"]), pc), [fs], False, None
    if op == "mfir":
        return "%s %s" % (" ".join(wu(ft, i, "(uint32_t)(int32_t)(int16_t)%s" % vi(is_)) for i in L) if ft else "", pv), [], False, None
    if op in ("div", "sqrt", "rsqrt"):
        k = {"div": 0, "sqrt": 1, "rsqrt": 2}[op]
        return ("vu.now = T + %%(C)d; div_(%d, %s, %s); %s" % (k, vfu(fs, l["fsf"]), vfu(ft, l["ftf"]), pv)), [fs, ft], False, None
    if op == "waitq": return "vu.now = T + %%(C)d; if (vu.q_pend) { vu.Q = vu.q_new; vu.q_pend = 0; } %s" % pv, [], False, None
    if op == "fmand": return "mac_commit_t_(T + %(P)d); " + setvi(it, "%s & vu.mac" % vi(is_), pc), [], True, None
    if op == "fmor": return "mac_commit_t_(T + %(P)d); " + setvi(it, "%s | vu.mac" % vi(is_), pc), [], True, None
    if op == "fmeq": return "mac_commit_t_(T + %(P)d); " + setvi(it, "%s == (vu.mac & 0xFFFF)" % vi(is_), pc), [], True, None
    if op == "fcand": return "clip_commit_t_(T + %%(P)d); i1_ = (vu.clip & 0x%06xu) != 0; %s" % (l["w"] & 0xFFFFFF, pv), [], True, None
    if op == "fcor": return "clip_commit_t_(T + %%(P)d); i1_ = ((vu.clip | 0x%06xu) & 0xFFFFFF) == 0xFFFFFF; %s" % (l["w"] & 0xFFFFFF, pv), [], True, None
    if op == "fceq": return "clip_commit_t_(T + %%(P)d); i1_ = (vu.clip & 0xFFFFFF) == 0x%06xu; %s" % (l["w"] & 0xFFFFFF, pv), [], True, None
    if op == "fcget": return "clip_commit_t_(T + %(P)d); " + setvi(it, "vu.clip & 0xFFF", pc), [], True, None
    if op == "fcset": return "vu.clip = 0x%06xu; vu.ncl = 0; %s" % (l["w"] & 0xFFFFFF, pv), [], False, None
    if op == "xtop": return setvi(it, "v_top", pc), [], False, None
    if op == "xitop": return setvi(it, "v_itop", pc), [], False, None
    if op == "xgkick": return "xgkick(%s); %s" % (vi(is_), pv), [], False, None
    def VI(r): return "(pvr == %d ? pvo : %s)" % (r, vi(r)) if r else "0"
    if op == "b": return pv, [], False, ("1", l["tgt"])
    if op == "jr": return pv, [], False, ("JR", VI(is_))
    if op == "ibeq": return pv, [], False, ("%s == %s" % (VI(it), VI(is_)), l["tgt"])
    if op == "ibne": return pv, [], False, ("%s != %s" % (VI(it), VI(is_)), l["tgt"])
    if op == "ibltz": return pv, [], False, ("(int16_t)%s < 0" % VI(is_), l["tgt"])
    if op == "ibgtz": return pv, [], False, ("(int16_t)%s > 0" % VI(is_), l["tgt"])
    if op == "iblez": return pv, [], False, ("(int16_t)%s <= 0" % VI(is_), l["tgt"])
    if op == "ibgez": return pv, [], False, ("(int16_t)%s >= 0" % VI(is_), l["tgt"])
    sys.exit("lower op %s" % op)

def q_reader(u): return u["kind"] == "ar" and u["src2"] == 2

# ---------------------------------------------------------------- blocks
w = out.write
w("/* hle_vu1_fast.h - generated by tools/vu1_fast.py from the game's VU1 code: keep private */\n")
w("static const struct { uint32_t lo, hi, hash; } fast_prog[%d] = {%s};\n" % (len(progs), ", ".join("{0x%03x, 0x%03x, 0x%08xu}" % p for p in progs)))
w("static uint64_t fast_mpg = ~0ull; static int fast_ok[%d]; static uint64_t st_fast_runs, st_fast_back;\n" % len(progs))
w("""static inline int32_t ftoi_(float d) { return d >= 2147483648.0f ? 0x7FFFFFFF : d <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)d; }
static inline uint32_t fres_u(float f) { return fres(f); }
/* fres in the float unit: the same bits (saturate exponent 255 to +-FLT_MAX, flush exponent 0 to +-0), no moves to
   the integer registers on the common path */
static inline __attribute__((always_inline)) float fresf_(float x)
{
    float a = fabsf(x);
    if (__builtin_expect(!(a <= FLT_MAX), 0)) return copysignf(FLT_MAX, x);
    if (__builtin_expect(a < FLT_MIN, 0)) { uint32_t u; memcpy(&u, &x, 4); u &= 0x80000000u; memcpy(&x, &u, 4); }
    return x;
}
/* MAC flags without the status queue (no program of the game reads the status flags: checked on the microcodes) */
static inline __attribute__((always_inline)) void mac_commit_(void) { while (vu.nmac && TDUE(vu.macq[vu.hmac].rdy)) { vu.mac = vu.macq[vu.hmac].v; vu.hmac = (vu.hmac + 1) & 7; vu.nmac--; } }
static inline __attribute__((always_inline)) void clip_commit_(void) { while (vu.ncl && TDUE(vu.clq[vu.hcl].rdy)) { vu.clip = vu.clq[vu.hcl].v; vu.hcl = (vu.hcl + 1) & 7; vu.ncl--; } }
static inline __attribute__((always_inline)) void mac_commit_t_(uint32_t t) { while (vu.nmac && (int32_t)(vu.macq[vu.hmac].rdy - t) <= 0) { vu.mac = vu.macq[vu.hmac].v; vu.hmac = (vu.hmac + 1) & 7; vu.nmac--; } }
static inline __attribute__((always_inline)) void clip_commit_t_(uint32_t t) { while (vu.ncl && (int32_t)(vu.clq[vu.hcl].rdy - t) <= 0) { vu.clip = vu.clq[vu.hcl].v; vu.hcl = (vu.hcl + 1) & 7; vu.ncl--; } }
static inline __attribute__((always_inline)) void push_mac_t_(uint32_t mac, uint32_t t) { if (vu.nmac == 8) mac_commit_t_(t); if (vu.nmac < 8) { int i = (vu.hmac + vu.nmac++) & 7; vu.macq[i].rdy = t + 4; vu.macq[i].v = mac; } }
static inline __attribute__((always_inline)) void push_clip_t_(uint32_t c, uint32_t t)    /* CLIP shifts the newest value */
{
    uint32_t prev = vu.ncl ? vu.clq[(vu.hcl + vu.ncl - 1) & 7].v : vu.clip;
    if (vu.ncl == 8) clip_commit_t_(t);
    if (vu.ncl < 8) { int i = (vu.hcl + vu.ncl++) & 7; vu.clq[i].rdy = t + 4; vu.clq[i].v = ((prev << 6) | c) & 0xFFFFFF; }
}
static inline __attribute__((always_inline)) void push_mac_ns_(uint32_t mac) { if (vu.nmac == 8) mac_commit_(); if (vu.nmac < 8) { int i = (vu.hmac + vu.nmac++) & 7; vu.macq[i].rdy = vu.now + 4; vu.macq[i].v = mac; } }
static inline uint32_t smax_(uint32_t a, uint32_t b) { int32_t x = (int32_t)a, y = (int32_t)b; int32_t kx = x < 0 ? (int32_t)(0x80000000u - a) : x, ky = y < 0 ? (int32_t)(0x80000000u - b) : y; return kx > ky ? a : b; }
static inline uint32_t smin_(uint32_t a, uint32_t b) { int32_t x = (int32_t)a, y = (int32_t)b; int32_t kx = x < 0 ? (int32_t)(0x80000000u - a) : x, ky = y < 0 ? (int32_t)(0x80000000u - b) : y; return kx < ky ? a : b; }
static void div_(int k, uint32_t su, uint32_t tu)
{
    if (vu.q_pend) { if (TLATE(vu.q_rdy)) vu.now = vu.q_rdy; vu.Q = vu.q_new; vu.q_pend = 0; }
    float a = fclamp(su), b = fclamp(tu), q;
    vu.status &= ~0x30u;
    if (k == 0) { if (b == 0) { q = (a == 0) ? 0 : ((su ^ tu) >> 31 ? -FLT_MAX : FLT_MAX); vu.status |= a == 0 ? 0x10 : 0x20; } else q = a / b; }
    else if (k == 1) q = sqrtf(fabsf(b));
    else { float s = sqrtf(fabsf(b)); if (s == 0) { q = (su >> 31) ? -FLT_MAX : FLT_MAX; vu.status |= 0x20; } else q = a / s; }
    uint32_t r = fres(q); memcpy(&q, &r, 4);
    vu.q_new = q; vu.q_pend = 1; vu.q_rdy = vu.now + (k == 2 ? 13 : 7);
}
""")
import io
head_w = w; buf = io.StringIO(); w = buf.write
w("static uint32_t fast_run(uint32_t pc)\n{\n")
w("    if (fast_mpg != st_mpg) { fast_mpg = st_mpg; for (int i = 0; i < %d; i++) fast_ok[i] = code_hash(fast_prog[i].lo) == fast_prog[i].hash; }\n" % len(progs))
w("    for (int i = 0; i < %d; i++) if (pc >= fast_prog[i].lo && pc < fast_prog[i].hi && !fast_ok[i]) return VU_NOAOT;\n" % len(progs))
w("    uint32_t T = vu.now; int pvr = -1; uint16_t pvo = 0; (void)pvo;\n    st_fast_runs++;\n")
w("    switch (pc) {\n" + "".join("    case 0x%03x: goto L%03x;\n" % (p, p) for p in sorted(leaders)) + "    default: return VU_NOAOT;\n    }\n")

def emit_ins(pc, C, P, ind):
    x = INS[pc]; u = x["u"]
    pre, com, _, _ = upper_c(u, pc)
    s = ind + "/* %03x */ " % pc
    if q_reader(u): s += "if (vu.q_pend && (int32_t)(vu.q_rdy - (T + %d)) <= 0) { vu.Q = vu.q_new; vu.q_pend = 0; } " % P
    br = None
    if x["loi"]:
        low = "memcpy(&vu.I, (const uint32_t[]){0x%08xu}, 4);" % x["lw"] + (" pvr = -1;" if pc in brpred else "")
    else:
        low, _, _, br = lower_c(x["l"], pc)
    body = "{ %s %s %s }" % (pre, low, com)
    return s + (body % dict(C=C, P=P)).replace("  ", " ") + "\n", br

def sched(pcs):
    """issue cycle of each instruction (block entry: all results ready), and the cycle before its stalls"""
    rdy = {}; qd = None; c = -1; res = []
    for pc in pcs:
        x = INS[pc]; u = x["u"]
        p = c + 1; c = p
        _, _, us, ud = upper_c(u, pc)
        ls = []
        if not x["loi"]:
            _, ls, _, _ = lower_c(x["l"], pc)
        for r in us + ls:
            if r and r in rdy and rdy[r] > c: c = rdy[r]
        if not x["loi"] and x["l"]["op"] in ("div", "sqrt", "rsqrt", "waitq") and qd is not None and qd > c: c = qd
        if not x["loi"] and x["l"]["op"] in ("div", "sqrt", "rsqrt"): qd = c + (13 if x["l"]["op"] == "rsqrt" else 7)
        if ud: rdy[ud] = c + 4
        res.append((pc, c, p))
    return res

done = set()
for L0 in sorted(leaders):
    # the block: from the leader to a branch + delay slot, an E bit + delay slot, the next leader or a gap
    pcs = []; pc = L0; endk = None
    while True:
        if pc not in INS: endk = ("gap", pc); break
        if pcs and pc in leaders: endk = ("fall", pc); break
        pcs.append(pc)
        if INS[pc]["e"]:
            pcs.append(pc + 1) if pc + 1 in INS else None
            endk = ("end", pc + 2); break
        if is_br(pc):
            if pc + 1 in INS: pcs.append(pc + 1)
            endk = ("br", pc); break
        pc += 1
    sc = sched(pcs)
    n = len(pcs); blen = (sc[-1][1] + 1) if sc else 0
    # MAC pushes no reader can see: a later push of the block is ready (4 cycles) before any reader in between and
    # by the block end (the next block's readers come later)
    pushes = [(pc, C) for pc, C, P in sc if pc in live]
    rd = [P for pc, C, P in sc if lower_reads_mac(pc)]
    for k in range(len(pushes) - 1):
        c0, c1 = pushes[k][1], pushes[k + 1][1]
        if c1 + 4 <= blen and not any(c0 + 4 <= r < c1 + 4 for r in rd):
            live.discard(pushes[k][0]); NDEAD[0] += 1
    w("L%03x: {\n" % L0)
    w("    st_steps += %d;\n" % n)
    br = None; dsl = None
    for pc, C, P in sc:
        if endk[0] == "br" and pc == endk[1]:
            t, b = emit_ins(pc, C, P, "    ")
            w(t); br = b
            w("    int take_ = %s;\n" % (b[1] if b[0] == "JR" else b[0])) if b[0] == "JR" else w("    int take_ = %s;\n" % b[0])
            continue
        t, _ = emit_ins(pc, C, P, "    "); w(t)
    w("    T += %d;\n" % blen)
    def go(t):
        if t in leaders: return "goto L%03x;" % t
        return "{ vu.now = T; st_fast_back++; return 0x%03x; }" % t
    if endk[0] == "end":
        w("    vu.now = T; vu.tpc = 0x%03x; return VU_END;\n" % endk[1])
    elif endk[0] in ("fall", "gap"):
        w("    %s\n" % go(endk[1]))
    else:
        bpc = endk[1]
        if br[0] == "JR":
            w("    switch (take_) {\n" + "".join("    case 0x%03x: goto L%03x;\n" % (t, t) for t in sorted(jr_t)) +
              "    default: vu.now = T; st_fast_back++; return take_ & 0x7FF;\n    }\n")
        else:
            w("    if (take_) %s\n" % go(br[1]))
            if br[0] != "1": w("    %s\n" % go(bpc + 2))
            else: w("    (void)take_;\n")
    w("}\n")
w("}\n")
text = buf.getvalue()
regs = sorted(USED)
decl = "    float %s;\n" % ", ".join("f%d_%d = vu.vf[%d].f[%d]" % (r, i, r, i) for r in regs for i in range(4))
USED_VI.add(1)
decl += "    uint16_t %s;\n" % ", ".join("i%d_ = vu.vi[%d]" % (r, r) for r in sorted(USED_VI))
decl += "    float a0_ = vu.acc.f[0], a1_ = vu.acc.f[1], a2_ = vu.acc.f[2], a3_ = vu.acc.f[3];\n"
wb = " ".join("vu.vf[%d].f[%d] = f%d_%d;" % (r, i, r, i) for r in regs for i in range(4)) + " " + " ".join("vu.vi[%d] = i%d_;" % (r, r) for r in sorted(USED_VI)) + " vu.acc.f[0] = a0_; vu.acc.f[1] = a1_; vu.acc.f[2] = a2_; vu.acc.f[3] = a3_;"
head_w("#define WB_ do { %s } while (0)\n" % wb)
head_w("static inline float asf_(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }\nstatic inline uint32_t asu_(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }\n")
text = text.replace("    uint32_t T = vu.now;", decl + "    uint32_t T = vu.now;", 1)
text = text.replace("vu.now = T;", "WB_; vu.now = T;")
head_w(text)
head_w("#undef WB_\n")
print("fast: %d instructions, %d blocks, %d live MAC pushes (%d more dropped: superseded in their block) of %d FMACs, %d jr targets" % (
    len(INS), len(leaders), len(live), NDEAD[0], sum(1 for x in INS.values() if sets_mac(x["u"])), len(jr_t)), file=sys.stderr)
