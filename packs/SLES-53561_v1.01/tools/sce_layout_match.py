"""sce_layout_match.py - name this game's Sony library functions without a signature database.

Idea: the reference pack (PS2RC games/SLES-52541_v2.01/analysis/sce_matches_v201.json) lists library function names,
addresses and sizes in another game linked with Sony's libraries. Inside one library the functions keep their order
and sizes when the library version is the same. For each library, every function start in THIS game's function set is
tried as an anchor; the anchor whose relative layout reproduces the most (offset, size) pairs wins. Per-function
results are accepted only when the size agrees exactly. Output: a JSON list like the reference (game-derived facts:
stays in the work folder).

    python sce_layout_match.py FUNCTIONS.pkl REF.json OUT.json
"""
import collections
import json
import pickle
import sys

sys.path.insert(0, sys.argv[4] if len(sys.argv) > 4 else ".")


def main():
    funcs = pickle.load(open(sys.argv[1], "rb"))
    ref = json.load(open(sys.argv[2]))
    size = {s: f.end - s for s, f in funcs.items()}
    starts = sorted(size)
    bylib = collections.defaultdict(list)
    for r in ref:
        if r["where"] == "main":
            bylib[r["lib"]].append((int(r["addr"], 16), r["size"], r["name"], len(r.get("ambiguous", ()))))
    out = []
    for lib, items in sorted(bylib.items()):
        items.sort()
        # split the reference into contiguous clusters (gap > 64 KB = a different link position)
        clusters, cur = [], [items[0]]
        for it in items[1:]:
            if it[0] - cur[-1][0] > 0x10000:
                clusters.append(cur)
                cur = [it]
            else:
                cur.append(it)
        clusters.append(cur)
        for cl in clusters:
            base = cl[0][0]
            rel = [(a - base, sz, nm, amb) for a, sz, nm, amb in cl]
            # repeated anchoring: each pass names the functions one shift explains, the rest get another pass
            # (a library version that inserted or resized a function moves the functions after it)
            left, passes, named = rel, 0, 0
            while left and passes < 12:
                passes += 1
                best, bscore = None, 0
                for s0 in starts:
                    sc = sum(1 for off, sz, _n, _a in left if sz >= 16 and size.get(s0 + off) == sz)
                    if sc > bscore:
                        best, bscore = s0, sc
                if best is None or bscore < 3:
                    break
                keep = []
                for off, sz, nm, amb in left:
                    a = best + off
                    if size.get(a) == sz:
                        out.append({"addr": "%08x" % a, "lib": lib, "name": nm, "size": sz, "where": "main",
                                    "ambiguous": amb, "pass": passes, "sa": base + off})
                        named += 1
                    else:
                        keep.append((off, sz, nm, amb))
                left = keep
            print("%-12s cluster @%08x (%d fns): %d named in %d passes" % (lib, base, len(rel), named, passes))
            # gaps between named neighbours: same count of functions on both sides -> pair them in order
            got = {o["name"] + "@%x" % (int(o["addr"], 16)): o for o in out if o["lib"] == lib}
            pairs = sorted((base + off, int(o["addr"], 16)) for off, sz, nm, amb in rel
                           for o in out if o["lib"] == lib and o["name"] == nm and o["size"] == sz
                           and int(o["addr"], 16) - (base + off) in ())
            sa_named = {}
            for o in out:
                if o["lib"] == lib and "sa" in o:
                    sa_named[o["sa"]] = int(o["addr"], 16)
            seq = sorted(sa_named.items())
            ok = all(seq[k][1] < seq[k + 1][1] for k in range(len(seq) - 1))
            interp = 0
            if ok:
                for k in range(len(seq) - 1):
                    (sa0, b0), (sa1, b1) = seq[k], seq[k + 1]
                    U = [r for r in rel if sa0 < base + r[0] < sa1]
                    V = [x for x in starts if b0 < x < b1]
                    if U and len(U) == len(V):
                        for (off, sz, nm, amb), a in zip(U, V):
                            out.append({"addr": "%08x" % a, "lib": lib, "name": nm, "size": size[a], "where": "main",
                                        "ambiguous": amb, "pass": "interp", "sa": base + off})
                            interp += 1
            print("%-12s   +%d by order between named neighbours (monotonic %s)" % (lib, interp, ok))
    json.dump(out, open(sys.argv[3], "w"), indent=1)
    print("named %d functions" % len(out))


if __name__ == "__main__":
    main()
