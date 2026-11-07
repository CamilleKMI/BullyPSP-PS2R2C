"""Conversion step: the game's own IOP sound modules translated to C on the player's machine from the player's own
disc (PS2RC's IOP static recompiler, python -m ps2rc.iop gen), into {work}/iopgen - never distributed.

    python convert/iopgen.py

Writes scream_gen.h (SYSTEM/SCREAM.IRX, symbol scream_irx) and audvag_gen.h (SYSTEM/AUDVAG.IRX, symbol audvag_irx),
plus PS2RC's generic IOP runtime (ps2rc/iop/rt) as headers: the pack's build copies the *.h of a source folder and
the pack's single unit iop/iop_snd_unit.c includes them (one unit: one IOP address-space size per build)."""
import os
import shutil
import subprocess
import sys

from common import DISC_DIR, PACK, ROOT, say

OUT = os.path.join(os.path.dirname(PACK.path("translate.functions")), "iopgen")
# (module, symbol, output, IOP load address): both are linked for 0x1000; AUDVAG is relocated above SCREAM
MODULES = [("SYSTEM/SCREAM.IRX", "scream_irx", "scream_gen.h", 0x1000), ("SYSTEM/AUDVAG.IRX", "audvag_irx", "audvag_gen.h", 0x40000)]
RT = [("iop_rt.c", "iop_rt_impl.h"), ("iop_rt_libs.c", "iop_rt_libs_impl.h"), ("iop_rt_ucontext.c", "iop_rt_ucontext_impl.h"),
      ("iop_rt.h", "iop_rt.h"), ("iop_rt_int.h", "iop_rt_int.h"), ("iop_rt_ucontext.h", "iop_rt_ucontext.h")]


def main():
    os.makedirs(OUT, exist_ok=True)
    for src, sym, out, base in MODULES:
        r = subprocess.run([sys.executable, "-m", "ps2rc.iop", "gen", os.path.join(DISC_DIR, src), "-o",
                            os.path.join(OUT, out), "--sym", sym, "--base", hex(base)], cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("iopgen: %s failed: %s" % (src, r.stderr.strip()[-400:]))
        say("iopgen: " + r.stdout.strip().splitlines()[-1])
    # exports of a translated module that another one imports (the runtime resolves only its built-in libraries):
    # number -> absolute entry address, for the glue's bindings (iop/iop_snd_unit.c)
    sys.path.insert(0, ROOT)
    from ps2rc.iop import irx
    with open(os.path.join(OUT, "iop_exports.h"), "w") as f:
        f.write("/* exports of the translated modules (convert/iopgen.py; facts of the player's disc) */\n")
        for src, sym, _out, base in MODULES:
            x = irx.load(os.path.join(DISC_DIR, src), base)
            for ex in x.exports:
                tab = ", ".join("0x%08xu" % ((base + a) if a else 0) for a in ex.fns)
                f.write("static const uint32_t EXP_%s_%s[%d] = {%s};\n" % (sym, ex.lib.strip(), len(ex.fns), tab))
    rtdir = os.path.join(ROOT, "ps2rc", "iop", "rt")
    for a, b in RT:
        shutil.copyfile(os.path.join(rtdir, a), os.path.join(OUT, b))
    say("iopgen: runtime copied -> %s" % OUT)


if __name__ == "__main__":
    main()
