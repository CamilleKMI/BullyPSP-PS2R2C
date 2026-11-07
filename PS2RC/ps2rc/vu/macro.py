"""VU0 macro mode (COP2) for the C back-end: C statements for the macro instructions and the EE<->VU0 transfers, and
the clean-lane analysis of back-end v3+ (operands known to be finite and not denormal skip the input clamp).

The C side lives in runtime/r5900_rt.h ("VU0 macro mode"): VU0 state in R5900Ctx.vu0, always_inline helpers
(vu0_fmac & co) that fold to straight-line code for the constant arguments emitted here. Semantics and the PCSX2
references: the "VU0 macro mode" section of r5900_rt.h.

Supported: FMAC add/sub/mul/madd/msub in every form (lane-wise, x/y/z/w broadcast, I, Q; Fd and ACC destinations),
VMOVE, VMR32, VABS, VNOP, VWAITQ, VRINIT/VRGET/VRNEXT/VRXOR, VCALLMS/VCALLMSR (rt_vu0_callms hook), LQC2, SQC2,
QMTC2, QMFC2, CFC2, CTC2 (+ .I/.NI). Everything else (MAX/MINI, OPMULA/OPMSUB, CLIP, FTOI/ITOF, DIV/SQRT/RSQRT,
integer and VU-memory instructions, BC2x, CTC2 to CMSAR1) is reported unsupported (stmt returns None).
"""
FIELDS = "xyzw"
_FAMILIES = ("vmadd", "vmsub", "vmul", "vadd", "vsub")          # longest prefixes first
_OP = {"vadd": "VU0_ADD", "vsub": "VU0_SUB", "vmul": "VU0_MUL", "vmadd": "VU0_MADD", "vmsub": "VU0_MSUB"}
_XFER = {"lqc2", "sqc2", "qmtc2", "qmfc2", "cfc2", "ctc2"}
ACC = 32                                                        # lane-state register index of ACC


def base_mnem(m):
    """Mnemonic without the dest suffix / interlock suffix: 'vmulax.xyz' -> 'vmulax', 'cfc2.i' -> 'cfc2'."""
    return m.split(".")[0]


def fmac(base):
    """(family, acc, kind) of an FMAC mnemonic base, else None. kind: '' lane-wise, 'x'..'w' broadcast, 'q', 'i'."""
    for fam in _FAMILIES:
        if base.startswith(fam):
            rest = base[len(fam):]
            acc = rest.startswith("a")
            kind = rest[1:] if acc else rest
            return (fam, acc, kind) if kind in ("", "x", "y", "z", "w", "q", "i") else None
    return None


def handles(insn):
    """True for the instructions this module translates (or rejects): COP2 macro mode and the VU0 transfers."""
    m = insn.mnem
    return m.startswith("v") or base_mnem(m) in _XFER


def _lanes(comps):
    return [FIELDS.index(ch) for ch in comps]


def _dest(comps):
    """C dest field mask: x = 8, y = 4, z = 2, w = 1 (the instruction's bits 24..21)."""
    return sum(8 >> i for i in _lanes(comps))


def _clean(st, n, i):
    return n == 0 or bool(st >> (4 * n + i) & 1)


def stmt(insn, st, R, W, mem_addr):
    """C statement(s) for one VU0 instruction, or None when unsupported. st: clean-lane state before the instruction
    (int bit 4*n+i = lane i of VF[n], ACC = n 32) or None (no knowledge: clamp every operand, clamp results).
    R/W/mem_addr: the back-end's GPR read / GPR write / effective-address helpers."""
    m, o = insn.mnem, insn.ops
    base = base_mnem(m)
    # ---- transfers
    if base == "lqc2":
        t = o[0][1]
        return "rt_lq(c, %s, &c->vu0.vf[%d]);" % (mem_addr(o[1]), t) if t else ";"
    if base == "sqc2":
        t = o[0][1]
        if t == 0:
            return "{ const R5900Reg128 t_ = vu0_vf(c, 0); rt_sq(c, %s, &t_); }" % mem_addr(o[1])
        return "rt_sq(c, %s, &c->vu0.vf[%d]);" % (mem_addr(o[1]), t)
    if base == "qmtc2":
        rt, d = o[0][1], o[1][1]
        if d == 0:
            return ";"
        if rt == 0:
            return "c->vu0.vf[%d].d[0] = 0; c->vu0.vf[%d].d[1] = 0;" % (d, d)
        return "c->vu0.vf[%d] = c->r[%d];" % (d, rt)
    if base == "qmfc2":
        rt, d = o[0][1], o[1][1]
        return "c->r[%d] = vu0_vf(c, %d);" % (rt, d) if rt else ";"
    if base == "cfc2":
        return W(o[0][1], "vu0_cfc2(c, %d)" % o[1][1]) or ";"
    if base == "ctc2":
        if o[1][1] == 31:                          # CMSAR1: starts a VU1 micro program (not modelled)
            return None
        return "vu0_ctc2(c, %d, (uint32_t)%s);" % (o[1][1], R(o[0][1]))
    # ---- macro instructions
    f = fmac(base)
    if f is not None:
        fam, acc, kind = f
        comps = o[0][1] if o[0][0] == "acc" else o[0][2]
        d = ACC if o[0][0] == "acc" else o[0][1]
        if d == 0:                                 # VF00 stays 0,0,0,1; only the MAC / status flags (vu0_setflags)
            s0, t0 = o[1][1], ("VU0_QREG" if kind == "q" else "VU0_IREG" if kind == "i" else o[2][1])
            bc0 = 0 if kind in ("q", "i") else (FIELDS.index(kind) if kind else -1)
            return "vu0_fmac(c, %s, 0, 0x%x, %d, %s, %d, 0, 0);" % (_OP[fam], _dest(comps), s0, t0, bc0)
        s = o[1][1]
        if kind == "q":
            t, bc = "VU0_QREG", 0
        elif kind == "i":
            t, bc = "VU0_IREG", 0
        else:
            t, bc = o[2][1], (FIELDS.index(kind) if kind else -1)
        cl, fast = 0, 0
        if st is not None:
            lanes = _lanes(comps)
            fast = 1
            if all(_clean(st, s, i) for i in lanes):
                cl |= 1
            if kind in ("q", "i"):
                pass
            elif kind and _clean(st, t, bc):
                cl |= 2
            elif not kind and all(_clean(st, t, i) for i in lanes):
                cl |= 2
            if fam in ("vmadd", "vmsub") and all(_clean(st, ACC, i) for i in lanes):
                cl |= 4
        return "vu0_fmac(c, %s, %s, 0x%x, %d, %s, %d, %d, %d);" % (
            _OP[fam], "VU0_ACC" if d == ACC else d, _dest(comps), s, t, bc, cl, fast)
    if base in ("vmove", "vmr32", "vabs"):
        t, s = o[0][1], o[1][1]
        if t == 0:
            return ";"
        return "vu0_move(c, 0x%x, %d, %d, %d, %d);" % (_dest(o[0][2]), t, s, base == "vmr32", base == "vabs")
    if base in ("vnop", "vwaitq"):
        return ";"
    x = _extra(base, o)
    if x is not None:
        return x
    if base in ("vrinit", "vrxor"):
        s, fsf = o[1][1], FIELDS.index(o[1][2])
        return "vu0_%s(c, vu0_lane(c, %d, %d));" % (base[1:], s, fsf)
    if base == "vrget":
        t = o[0][1]
        return "vu0_fill(c, 0x%x, %d, c->vu0.vi[20]);" % (_dest(o[0][2]), t) if t else ";"
    if base == "vrnext":                           # the LFSR advances even when the result goes to VF00 (microVU)
        t = o[0][1]
        if t == 0:
            return "(void)vu0_rnext(c);"
        return "{ uint32_t r_ = vu0_rnext(c); vu0_fill(c, 0x%x, %d, r_); }" % (_dest(o[0][2]), t)
    # flush kind "vcm": the back-end's full ("fatal") flush, or with the lever vcm only the VU0 lanes
    if base == "vcallms":
        return "{ /*FLUSH@i%08x|vcm*/ rt_vu0_callms(c, 0x%08xu, 0x%xu); }" % (insn.pc, insn.pc, o[0][1])
    if base == "vcallmsr":
        return ("{ /*FLUSH@i%08x|vcm*/ rt_vu0_callms(c, 0x%08xu, (c->vu0.vi[27] & 0xFFFFu) * 8u); }"
                % (insn.pc, insn.pc))
    return None


def _extra(base, o):
    """The second instruction set (SLES-53561 port): ITOF/FTOI, DIV/SQRT/RSQRT, OPMULA/OPMSUB, MAX/MINI, CLIPw."""
    for pre, ftoi in (("vitof", 0), ("vftoi", 1)):
        if base.startswith(pre) and base[len(pre):] in ("0", "4", "12", "15"):
            return "vu0_cvt(c, 0x%x, %d, %d, %d, %s);" % (_dest(o[0][2]), o[0][1], o[1][1], ftoi, base[len(pre):])
    if base == "vdiv":
        return "vu0_div(c, 0, vu0_lane(c, %d, %d), vu0_lane(c, %d, %d));" % (
            o[1][1], FIELDS.index(o[1][2]), o[2][1], FIELDS.index(o[2][2]))
    if base == "vsqrt":
        return "vu0_div(c, 1, 0u, vu0_lane(c, %d, %d));" % (o[1][1], FIELDS.index(o[1][2]))
    if base == "vrsqrt":
        return "vu0_div(c, 2, vu0_lane(c, %d, %d), vu0_lane(c, %d, %d));" % (
            o[1][1], FIELDS.index(o[1][2]), o[2][1], FIELDS.index(o[2][2]))
    if base == "vopmula":
        return "vu0_opm(c, 0, 0x%x, 0, %d, %d);" % (_dest(o[0][1]), o[1][1], o[2][1])
    if base == "vopmsub":
        return "vu0_opm(c, 1, 0x%x, %d, %d, %d);" % (_dest(o[0][2]), o[0][1], o[1][1], o[2][1])
    for pre, mx in (("vmax", 1), ("vmini", 0)):
        if base.startswith(pre) and base[len(pre):] in ("", "x", "y", "z", "w", "i"):
            k = base[len(pre):]
            if k == "i":
                t, bc = "VU0_IREG", 0
            else:
                t, bc = o[2][1], (FIELDS.index(k) if k else -1)
            return "vu0_minmax(c, %d, 0x%x, %d, %d, %s, %d);" % (mx, _dest(o[0][2]), o[0][1], o[1][1], t, bc)
    if base in ("vclipw",):
        return "vu0_clipw(c, %d, vu0_lane(c, %d, %d));" % (o[0][1], o[1][1], FIELDS.index(o[1][2]))
    return None


def _extra_loc(base, o):
    """_extra on the lane locals (lever vfloc): ITOF/FTOI, DIV/SQRT/RSQRT, MAX/MINI, OPMULA/OPMSUB, CLIPw; same
    helpers per lane as the c->vu0 versions (r5900_rt.h), all lanes computed before any is written."""
    if _NOXLOC:
        return None
    for pre, ftoi in (("vitof", 0), ("vftoi", 1)):
        if base.startswith(pre) and base[len(pre):] in ("0", "4", "12", "15"):
            t, s, n = o[0][1], o[1][1], base[len(pre):]
            if not t:
                return ";"
            ls = _lanes(o[0][2])
            fn = "vu0_ftoi1" if ftoi else "vu0_itof1"
            return "{ %s %s }" % (" ".join("uint32_t x%d_ = %s(%s, %s);" % (i, fn, _lv(s, i), n) for i in ls),
                                  " ".join("v%d_%d = x%d_;" % (t, i, i) for i in ls))
    if base == "vdiv":
        return "vu0_div(c, 0, %s, %s);" % (_lv(o[1][1], FIELDS.index(o[1][2])), _lv(o[2][1], FIELDS.index(o[2][2])))
    if base == "vsqrt":
        return "vu0_div(c, 1, 0u, %s);" % _lv(o[1][1], FIELDS.index(o[1][2]))
    if base == "vrsqrt":
        return "vu0_div(c, 2, %s, %s);" % (_lv(o[1][1], FIELDS.index(o[1][2])), _lv(o[2][1], FIELDS.index(o[2][2])))
    for pre, mx in (("vmax", 1), ("vmini", 0)):
        if base.startswith(pre) and base[len(pre):] in ("", "x", "y", "z", "w", "i"):
            k = base[len(pre):]
            d, s = o[0][1], o[1][1]
            if not d:
                return ";"
            ls = _lanes(o[0][2])
            def tl(i):
                if k == "i":
                    return "c->vu0.vi[21]"
                return _lv(o[2][1], FIELDS.index(k) if k else i)
            pick = "%s(vu0_ord(%s) >= vu0_ord(%s) ? %s : %s)"
            xs = []
            for i in ls:
                a, b = _lv(s, i), tl(i)
                xs.append("uint32_t x%d_ = vu0_ord(%s) >= vu0_ord(%s) ? %s : %s;" % (i, a, b, a, b) if mx else
                          "uint32_t x%d_ = vu0_ord(%s) >= vu0_ord(%s) ? %s : %s;" % (i, a, b, b, a))
            return "{ %s %s }" % (" ".join(xs), " ".join("v%d_%d = x%d_;" % (d, i, i) for i in ls))
    if base in ("vopmula", "vopmsub"):
        sub = base == "vopmsub"
        if sub:
            d, s, t, comps = o[0][1], o[1][1], o[2][1], o[0][2]
        else:
            d, s, t, comps = None, o[1][1], o[2][1], o[0][1]
        a1, a2 = (1, 2, 0), (2, 0, 1)
        xs = []
        for i in range(3):
            p = "u2f(vu0_clamp(%s)) * u2f(vu0_clamp(%s))" % (_lv(s, a1[i]), _lv(t, a2[i]))
            if sub:
                xs.append("uint32_t x%d_ = vu0_clamp(f2u(u2f(vu0_clamp(va_%d)) - u2f(vu0_clamp(f2u(%s)))));" % (i, i, p))
            else:
                xs.append("uint32_t x%d_ = vu0_clamp(f2u(%s));" % (i, p))
        ls = [i for i in _lanes(comps) if i < 3]
        if sub and not d:
            return ";"
        dst = (lambda i: "v%d_%d" % (d, i)) if sub else (lambda i: "va_%d" % i)
        return "{ %s %s }" % (" ".join(xs), " ".join("%s = x%d_;" % (dst(i), i) for i in ls))
    if base == "vclipw":
        s, w = o[0][1], _lv(o[1][1], FIELDS.index(o[1][2]))
        return "vu0_clipw_l(c, %s, %s, %s, %s);" % (_lv(s, 0), _lv(s, 1), _lv(s, 2), w)
    return None


_NOXLOC = bool(__import__("os").environ.get("PS2RC_NOXLOC"))   # the second set through c->vu0 (flush / reload) as before


def extra_writes(base, o):
    """VF registers (32 = ACC) the second instruction set writes (None: not one of them)."""
    if base.startswith(("vitof", "vftoi", "vmax", "vmini")):
        return {o[0][1]} - {0}
    if base == "vopmula":
        return {ACC}
    if base == "vopmsub":
        return {o[0][1]} - {0}
    if base in ("vdiv", "vsqrt", "vrsqrt", "vclipw"):
        return set()
    return None


# ---------------------------------------------------------------- lever vfloc
# VF01-VF31 and ACC lanes in C locals v<n>_<lane> / va_<lane> (uint32_t raw bits) inside an ABI-mode function; the
# back-end writes the lanes the function writes back to c->vu0 at every boundary (calls, returns, syscalls, fatal
# points, VCALLMS) and reloads them after calls and VCALLMS - the context holds the guest VU0 state wherever anything
# else can observe it. VF00, I, Q, R and the VI registers stay in the context.
def _lv(n, i):
    """Local (or constant) for lane i of VF[n]."""
    if n == 0:
        return "0x3F800000u" if i == 3 else "0u"
    return "v%d_%d" % (n, i)


def vf_writes(insn):
    """VF registers (32 = ACC) an instruction writes; -1 for VCALLMS (the micro subroutine may write any)."""
    m, o = insn.mnem, insn.ops
    base = base_mnem(m)
    if base in ("vcallms", "vcallmsr"):
        return -1
    if extra_writes(base, o) is not None:            # the second set: on the locals (_extra_loc) unless PS2RC_NOXLOC
        return -1 if _NOXLOC or _extra_loc(base, o) is None else extra_writes(base, o)
    if base == "lqc2":
        return {o[0][1]} - {0}
    if base == "qmtc2":
        return {o[1][1]} - {0}
    f = fmac(base) if m.startswith("v") else None
    if f is not None:
        return {ACC if o[0][0] == "acc" else o[0][1]} - {0}
    if base in ("vmove", "vmr32", "vabs", "vrget", "vrnext"):
        return {o[0][1]} - {0}
    x = extra_writes(base, o)
    return x if x is not None else set()


def stmt_loc(insn, st, R, W, mem_addr):
    """stmt() with the VF / ACC lanes in C locals (lever vfloc)."""
    m, o = insn.mnem, insn.ops
    base = base_mnem(m)
    if base == "lqc2":
        t = o[0][1]
        if not t:
            return ";"
        return "{ R5900Reg128 q_; rt_lq(c, %s, &q_); %s }" % (
            mem_addr(o[1]), " ".join("v%d_%d = q_.w[%d];" % (t, i, i) for i in range(4)))
    if base == "sqc2":
        t = o[0][1]
        return "{ R5900Reg128 q_; %s rt_sq(c, %s, &q_); }" % (
            " ".join("q_.w[%d] = %s;" % (i, _lv(t, i)) for i in range(4)), mem_addr(o[1]))
    if base == "qmtc2":
        rt, d = o[0][1], o[1][1]
        if d == 0:
            return ";"
        return " ".join("v%d_%d = %s;" % (d, i, "0u" if rt == 0 else "c->r[%d].w[%d]" % (rt, i)) for i in range(4))
    if base == "qmfc2":
        rt, d = o[0][1], o[1][1]
        return " ".join("c->r[%d].w[%d] = %s;" % (rt, i, _lv(d, i)) for i in range(4)) if rt else ";"
    x = _extra_loc(base, o)
    if x is not None:                                    # the second set on the locals (no flush / reload)
        return x
    x = _extra(base, o)
    if x is not None:                                    # the second set (SLES-53561 port): run on c->vu0 like a
        return "{ /*FLUSH@i%08x|vcm*/ %s /*VRELOAD*/ }" % (insn.pc, x)   # micro call - dirty lanes out, all back in
    if base in ("cfc2", "ctc2", "vnop", "vwaitq", "vcallms", "vcallmsr"):
        s = stmt(insn, st, R, W, mem_addr)
        if s is not None and base in ("vcallms", "vcallmsr"):
            s = s[:-1] + "/*VRELOAD*/ }"                 # the micro subroutine works on c->vu0
        return s
    f = fmac(base)
    if f is not None:
        fam, acc, kind = f
        comps = o[0][1] if o[0][0] == "acc" else o[0][2]
        d = ACC if o[0][0] == "acc" else o[0][1]
        if d == 0:                                 # only the flags: computed from the locals, VF00 unchanged
            s0 = o[1][1]
            if kind in ("q", "i"):
                t0, bc0 = ("c->vu0.vi[22]" if kind == "q" else "c->vu0.vi[21]"), 0
            else:
                t0, bc0 = o[2][1], (FIELDS.index(kind) if kind else -1)
            lanes = _lanes(comps)
            xs = []
            for i in range(4):
                if i not in lanes:
                    xs.append("0u"); continue
                tv = t0 if isinstance(t0, str) else _lv(t0, i if bc0 < 0 else bc0)
                av = ("va_%d" % i) if fam in ("vmadd", "vmsub") else "0u"
                xs.append("vu0_fmac1(%s, %s, %s, %s, 0, 0)" % (_OP[fam], av, _lv(s0, i), tv))
            return "vu0_setflags(c, 0x%x, %s);" % (_dest(comps), ", ".join(xs))
        s = o[1][1]
        if kind == "q":
            t, bc = "Q", 0
        elif kind == "i":
            t, bc = "I", 0
        else:
            t, bc = o[2][1], (FIELDS.index(kind) if kind else -1)
        cl, fast = 0, 0
        if st is not None:
            lanes = _lanes(comps)
            fast = 1
            if all(_clean(st, s, i) for i in lanes):
                cl |= 1
            if kind in ("q", "i"):
                pass
            elif kind and _clean(st, t, bc):
                cl |= 2
            elif not kind and all(_clean(st, t, i) for i in lanes):
                cl |= 2
            if fam in ("vmadd", "vmsub") and all(_clean(st, ACC, i) for i in lanes):
                cl |= 4
        calc, assign = [], []
        for i in _lanes(comps):
            tv = ("c->vu0.vi[21]" if t == "I" else "c->vu0.vi[22]" if t == "Q" else
                  _lv(t, i if bc < 0 else bc))
            calc.append("x%d_ = vu0_fmac1(%s, va_%d, %s, %s, %d, %d)" % (i, _OP[fam], i, _lv(s, i), tv, cl, fast)
                        if fam in ("vmadd", "vmsub") else
                        "x%d_ = vu0_fmac1(%s, 0u, %s, %s, %d, %d)" % (i, _OP[fam], _lv(s, i), tv, cl, fast))
            assign.append("%s = x%d_;" % ("va_%d" % i if d == ACC else "v%d_%d" % (d, i), i))
        return "{ uint32_t %s; %s }" % (", ".join(calc), " ".join(assign))
    if base in ("vmove", "vmr32", "vabs"):
        t, s = o[0][1], o[1][1]
        if t == 0:
            return ";"
        rot = 1 if base == "vmr32" else 0
        mask = " & 0x7FFFFFFFu" if base == "vabs" else ""
        lanes = _lanes(o[0][2])
        return "{ uint32_t %s; %s }" % (", ".join("x%d_ = %s%s" % (i, _lv(s, (i + rot) & 3), mask) for i in lanes),
                                       " ".join("v%d_%d = x%d_;" % (t, i, i) for i in lanes))
    if base in ("vrinit", "vrxor"):
        s, fsf = o[1][1], FIELDS.index(o[1][2])
        return "vu0_%s(c, %s);" % (base[1:], _lv(s, fsf))
    if base == "vrget":
        t = o[0][1]
        return " ".join("v%d_%d = c->vu0.vi[20];" % (t, i) for i in _lanes(o[0][2])) if t else ";"
    if base == "vrnext":
        t = o[0][1]
        if t == 0:
            return "(void)vu0_rnext(c);"
        return "{ uint32_t r_ = vu0_rnext(c); %s }" % " ".join("v%d_%d = r_;" % (t, i) for i in _lanes(o[0][2]))
    return None


# ---------------------------------------------------------------- clean-lane analysis (back-end v3+)
def transfer(insn, st):
    """Clean-lane state after `insn`. Clean = finite and not denormal (the input clamp is the identity): FMAC results
    (round-toward-zero cannot overflow to Inf, flush-to-zero removes denormals), R-register values (1.0 + mantissa),
    moves of clean lanes. LQC2/QMTC2 load unknown bits; a micro subroutine may write anything."""
    m, o = insn.mnem, insn.ops
    base = base_mnem(m)
    if base == "lqc2" and o[0][1]:
        return st & ~(0xF << 4 * o[0][1])
    if base == "qmtc2" and o[1][1]:
        return st & ~(0xF << 4 * o[1][1])
    if base in ("vcallms", "vcallmsr"):
        return 0
    if not m.startswith("v"):
        return st
    if fmac(base) is not None:
        n = ACC if o[0][0] == "acc" else o[0][1]
        comps = o[0][1] if o[0][0] == "acc" else o[0][2]
        if n == 0:
            return st
        return st | sum(1 << (4 * n + i) for i in _lanes(comps))
    if base in ("vmove", "vmr32", "vabs"):
        t, s = o[0][1], o[1][1]
        if t == 0:
            return st
        rot = 1 if base == "vmr32" else 0
        new = st
        for i in _lanes(o[0][2]):
            bit = 1 << (4 * t + i)
            new = new | bit if _clean(st, s, (i + rot) & 3) else new & ~bit
        return new
    if base in ("vrget", "vrnext") and o[0][1]:
        return st | sum(1 << (4 * o[0][1] + i) for i in _lanes(o[0][2]))
    x = extra_writes(base, o)
    if x:
        for n in x:
            st &= ~(0xF << 4 * n)
    return st


def clean_states(img, start, addrs, jump_tables, func_starts):
    """{address: clean-lane state before the instruction} (None: unreachable) for one function, or None when the
    function has no VU0 instruction. Function entry and every call return start with nothing known."""
    import heapq

    from ..backend_c import fn_cfg
    if not any(img.word(a) >> 26 in (0x12, 0x36, 0x3E) for a in addrs):     # no COP2 / LQC2 / SQC2 word
        return None
    f = fn_cfg(img, start, addrs, jump_tables, func_starts)
    din = {a: None for a in f.addrs}
    if start not in din:
        return din
    din[start] = 0
    work, pending = [start], {start}
    while work:
        a = heapq.heappop(work)
        pending.discard(a)
        out = transfer(f.insn[a], din[a])
        for t, e in f.succ[a]:
            v = 0 if e is not None else out
            cur = din[t]
            new = v if cur is None else cur & v
            if new != cur:
                din[t] = new
                if t not in pending:
                    pending.add(t)
                    heapq.heappush(work, t)
    return din
