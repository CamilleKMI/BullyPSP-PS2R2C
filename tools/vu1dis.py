#!/usr/bin/env python3
"""vu1dis.py CODE.bin [START [END]] - full VU micro-mode disassembly (upper and lower words with every operand),
branch targets as labels. Uses PS2RC's decoder (ps2rc.vu.micro_gen_isa) for the fields."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "PS2RC"))
from ps2rc.vu.micro_gen_isa import decode_program, dest_str, XYZW, BRANCHES


def up_text(u):
    if u.name in ("NOP", "?"):
        return u.name.lower()
    d = dest_str(u.dest)
    if u.base == "CLIP":
        return "clipw.xyz vf%02d, vf%02dw" % (u.fs, u.ft)
    if u.base in ("ITOF", "FTOI", "ABS"):
        return "%s.%s vf%02d, vf%02d" % (u.name.lower(), d, u.ft, u.fs)
    dst = "ACC" if (u.acc_dst or u.base == "OPMULA") else "vf%02d" % u.fd
    if u.src2 == "i":
        t = "I"
    elif u.src2 == "q":
        t = "Q"
    else:
        t = "vf%02d%s" % (u.ft, XYZW[u.bc] if u.bc >= 0 else "")
    return "%s.%s %s, vf%02d, %s" % (u.name.lower(), d, dst, u.fs, t)


def lo_text(m):
    lo = m.lo
    n = lo.name
    d = dest_str(lo.dest)
    tgt = lambda: "L%03x" % ((m.index + 1 + lo.imm) & 0x7FF)
    if n in ("LQ",):
        return "lq.%s vf%02d, %d(vi%02d)" % (d, lo.ft, lo.imm, lo.is_)
    if n == "SQ":
        return "sq.%s vf%02d, %d(vi%02d)" % (d, lo.fs, lo.imm, lo.it)
    if n == "ILW":
        return "ilw.%s vi%02d, %d(vi%02d)" % (d, lo.it, lo.imm, lo.is_)
    if n == "ISW":
        return "isw.%s vi%02d, %d(vi%02d)" % (d, lo.it, lo.imm, lo.is_)
    if n in ("LQI", "LQD"):
        return "%s.%s vf%02d, (vi%02d%s)" % (n.lower(), d, lo.ft, lo.is_, "++" if n == "LQI" else "--")
    if n in ("SQI", "SQD"):
        return "%s.%s vf%02d, (vi%02d%s)" % (n.lower(), d, lo.fs, lo.it, "++" if n == "SQI" else "--")
    if n in ("ILWR", "ISWR"):
        return "%s.%s vi%02d, (vi%02d)" % (n.lower(), d, lo.it, lo.is_)
    if n in ("IADDIU", "ISUBIU"):
        return "%s vi%02d, vi%02d, 0x%x" % (n.lower(), lo.it, lo.is_, lo.imm)
    if n == "IADDI":
        return "iaddi vi%02d, vi%02d, %d" % (lo.it, lo.is_, lo.imm)
    if n in ("IADD", "ISUB", "IAND", "IOR"):
        return "%s vi%02d, vi%02d, vi%02d" % (n.lower(), lo.id_, lo.is_, lo.it)
    if n in ("MOVE", "MR32"):
        return "%s.%s vf%02d, vf%02d" % (n.lower(), d, lo.ft, lo.fs)
    if n == "MFIR":
        return "mfir.%s vf%02d, vi%02d" % (d, lo.ft, lo.is_)
    if n == "MTIR":
        return "mtir vi%02d, vf%02d%s" % (lo.it, lo.fs, XYZW[lo.fsf])
    if n == "MFP":
        return "mfp.%s vf%02d, P" % (d, lo.ft)
    if n in ("DIV", "RSQRT"):
        return "%s Q, vf%02d%s, vf%02d%s" % (n.lower(), lo.fs, XYZW[lo.fsf], lo.ft, XYZW[lo.ftf])
    if n == "SQRT":
        return "sqrt Q, vf%02d%s" % (lo.ft, XYZW[lo.ftf])
    if n in ("XTOP", "XITOP"):
        return "%s vi%02d" % (n.lower(), lo.it)
    if n == "XGKICK":
        return "xgkick vi%02d" % lo.is_
    if n in ("ESADD", "ERSADD", "ELENG", "ERLENG", "EATANxy", "EATANxz", "ESUM"):
        return "%s P, vf%02d" % (n.lower(), lo.fs)
    if n in ("ESQRT", "ERSQRT", "ERCPR", "ESIN", "EATAN", "EEXP"):
        return "%s P, vf%02d%s" % (n.lower(), lo.fs, XYZW[lo.fsf])
    if n in ("FCEQ", "FCSET", "FCAND", "FCOR"):
        return "%s vi01, 0x%06x" % (n.lower(), lo.imm)
    if n in ("FSEQ", "FSSET", "FSAND", "FSOR"):
        return "%s vi%02d, 0x%03x" % (n.lower(), lo.it, lo.imm)
    if n in ("FMEQ", "FMAND", "FMOR"):
        return "%s vi%02d, vi%02d" % (n.lower(), lo.it, lo.is_)
    if n == "FCGET":
        return "fcget vi%02d" % lo.it
    if n in ("B",):
        return "b %s" % tgt()
    if n == "BAL":
        return "bal vi%02d, %s" % (lo.it, tgt())
    if n == "JR":
        return "jr vi%02d" % lo.is_
    if n == "JALR":
        return "jalr vi%02d, vi%02d" % (lo.it, lo.is_)
    if n in ("IBEQ", "IBNE"):
        return "%s vi%02d, vi%02d, %s" % (n.lower(), lo.it, lo.is_, tgt())
    if n in ("IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
        return "%s vi%02d, %s" % (n.lower(), lo.is_, tgt())
    if n in ("WAITQ", "WAITP", "RNEXT", "RGET", "RINIT", "RXOR"):
        return n.lower() + ("" if n in ("WAITQ", "WAITP") else " ...")
    return n.lower()


def main():
    code = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 16) if len(sys.argv) > 3 else len(code) // 8
    prog = decode_program(code)
    targets = set()
    for m in prog:
        if m.lo is not None and m.lo.name in BRANCHES and m.lo.name not in ("JR", "JALR"):
            targets.add((m.index + 1 + m.lo.imm) & 0x7FF)
    for m in prog[start:end]:
        lab = ("L%03x:" % m.index) if m.index in targets else ""
        lo = ("loi 0x%08x" % m.lower) if m.ibit else lo_text(m)
        fl = "".join(c for c, b in (("I", m.ibit), ("E", m.ebit), ("M", m.mbit), ("D", m.dbit), ("T", m.tbit)) if b)
        print("%-6s%03x: %-38s %s%s" % (lab, m.index, up_text(m.up), lo, ("   [%s]" % fl) if fl else ""))


if __name__ == "__main__":
    main()
