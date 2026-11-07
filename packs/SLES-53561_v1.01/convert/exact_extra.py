"""Conversion step: adds the pack's lists/exact_extra.txt (one hex address per line, '#' comments) to the
[translate] non_abi file's exact_starts of the main unit - functions the ABI-mode back-end must translate exact
(found by bisection against the exact v0 build). Run after convert/non_abi.py."""
import json
import os

from common import PACK, PACK_DIR, say

path = PACK.path("translate.non_abi")
lst = os.path.join(PACK_DIR, "lists", "exact_extra.txt")
d = json.load(open(path))
have = set(d["exact_starts"]["main"])
add = []
if os.path.exists(lst):
    for ln in open(lst):
        w = ln.split("#")[0].strip().lower()
        if w and w not in have:
            add.append(w.zfill(8))
d["exact_starts"]["main"] = sorted(have | set(add))
json.dump(d, open(path, "w"), indent=1)
say("exact_extra: +%d functions exact (%d in all) -> %s" % (len(add), len(d["exact_starts"]["main"]), path))
