"""make_hooks.py - write the Sony-library part of hooks.def from the alignment results (work/analysis/al_*.json):
only the names the core HLE implements (HLE_SCE(...) in PS2RC hle/core/*.c), plus the pack's manual list.

    python make_hooks.py PS2RC_DIR WORK_ANALYSIS_DIR OUT_HOOKS_PART
"""
import glob
import json
import os
import re
import sys

ps2rc, adir, out = sys.argv[1:4]
core = set()
for p in glob.glob(os.path.join(ps2rc, "hle", "core", "*.c")):
    core |= set(re.findall(r"HLE_SCE\((\w+)\)", open(p).read()))
found = {}
for p in sorted(glob.glob(os.path.join(adir, "al_*.json"))):
    for r in json.load(open(p)):
        if r["name"] in core and r["name"] not in found:
            found[r["name"]] = (r["addr"], r["lib"], "exact" if r["exact"] else "size differs, order-aligned")
# manual corrections (checked by disassembly)
MANUAL = {"_InitSys": ("0011e8d8", "libkernl", "calls InitThread, tail clears the 512-byte table"),
          "sceMcInit": ("001551c8", "libmc", "binds 0x80000400"),
          "sceMcEnd": ("00155460", "libmc", "order after sceMcInit")}
found.update(MANUAL)
order = ["libkernl", "libc", "libcdvd", "libpad", "libmc", "libgraph", "libdma"]
with open(out, "w") as f:
    for lib in order:
        names = sorted((v[0], k, v[2]) for k, v in found.items() if v[1] == lib)
        if not names:
            continue
        f.write("/* ---- %s */\n" % lib)
        for a, n, why in names:
            f.write("HOOK(%s, %s)%s\n" % (a, n, "" if why == "exact" else "    /* %s */" % why))
missing = sorted(core - set(found))
print("%d hooks; core names not found: %s" % (len(found), " ".join(missing)))
