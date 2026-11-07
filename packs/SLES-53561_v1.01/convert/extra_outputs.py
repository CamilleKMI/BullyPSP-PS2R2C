"""Conversion step: functions the non-ABI detector keeps exact only for the reason 2-output (a caller reads, after the
call, an argument register the function hands back) move from the [translate] non_abi file's exact_starts to its
"extra_outputs" {unit: {start: [register names]}}: the ABI-mode back-end translates them in ABI mode and passes those
registers through the context at their returns and around their direct calls (backend_c.XOUT). Kept exact: functions
with any other reason, address-taken ones (an indirect call would not reload the register), those of
lists/exact_extra.txt, and outputs other than caller-saved GPRs. Run after convert/non_abi.py and exact_extra.py."""
import json
import os

from common import PACK, PACK_DIR, say

OK = {"at", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9"}

path = PACK.path("translate.non_abi")
lst = os.path.join(PACK_DIR, "lists", "exact_extra.txt")
forced = set()
if os.path.exists(lst):
    for ln in open(lst):
        w = ln.split("#")[0].strip().lower()
        if w:
            forced.add(w.zfill(8))
d = json.load(open(path))
taken = set(d.get("address_taken_exact", ()))
xo = {}
for key, v in d["functions"].items():
    unit, a = key.rsplit(":", 1)
    if v["reasons"] != ["2-output"] or key in taken or (unit == "main" and a in forced):
        continue
    regs = sorted(v["details"]["2-output"])
    if not set(regs) <= OK:
        continue
    xo.setdefault(unit, {})[a] = regs
for unit, fs in xo.items():
    d["exact_starts"][unit] = [s for s in d["exact_starts"][unit] if s not in fs]
d["extra_outputs"] = xo
json.dump(d, open(path, "w"), indent=1)
say("extra_outputs: %d functions leave the exact set (%s)" % (sum(len(f) for f in xo.values()),
                                                            ", ".join("%s %d" % (u, len(f)) for u, f in xo.items())))
