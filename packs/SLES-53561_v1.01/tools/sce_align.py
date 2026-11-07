"""sce_align.py - name library functions by aligning the reference game's library layout (names + sizes, from
PS2RC's SLES-52541 pack analysis) with this game's function sequence in one address range (global sequence
alignment on function sizes: equal size = match, otherwise a substitution or a gap). Prints name -> address with the
evidence (exact size or substituted). Game-derived facts: the output stays in the work folder.

    python sce_align.py FUNCTIONS.pkl REF.json LIB REF_LO REF_HI LO HI [OUT.json]
"""
import json
import pickle
import os
import sys
sys.path.insert(0, os.environ.get("PS2RC_ROOT", "."))


def main():
    funcs = pickle.load(open(sys.argv[1], "rb"))
    ref = json.load(open(sys.argv[2]))
    lib = sys.argv[3]
    rlo, rhi, lo, hi = (int(x, 16) for x in sys.argv[4:8])
    A = sorted((int(x["addr"], 16), x["size"], x["name"]) for x in ref
               if (lib == "*" or x["lib"] == lib) and rlo <= int(x["addr"], 16) < rhi and x["where"] == "main")
    seen = set(); A = [a for a in A if not (a[0] in seen or seen.add(a[0]))]
    B = [(s, funcs[s].end - s) for s in sorted(funcs) if lo <= s < hi]
    n, m = len(A), len(B)
    MATCH, SUB, GAP = 4, -1, -2
    S = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        S[i][0] = i * GAP
    for j in range(1, m + 1):
        S[0][j] = 0 if True else j * GAP          # free leading gaps in B (the range may start early)
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            sc = MATCH if A[i - 1][1] == B[j - 1][1] else SUB
            S[i][j] = max(S[i - 1][j - 1] + sc, S[i - 1][j] + GAP, S[i][j - 1] + GAP)
    j = max(range(m + 1), key=lambda k: S[n][k])  # free trailing gaps in B
    i, out = n, []
    while i > 0 and j > 0:
        sc = MATCH if A[i - 1][1] == B[j - 1][1] else SUB
        if S[i][j] == S[i - 1][j - 1] + sc:
            out.append((A[i - 1], B[j - 1], sc == MATCH)); i -= 1; j -= 1
        elif S[i][j] == S[i - 1][j] + GAP:
            out.append((A[i - 1], None, False)); i -= 1
        else:
            j -= 1
    out.reverse()
    res = []
    for a, b, exact in out:
        print("%-32s ref %08x %5d -> %s" % (a[2], a[0], a[1], ("%08x %5d %s" % (b[0], b[1], "exact" if exact else "SUBST"))
                                             if b else "-"))
        if b:
            res.append({"addr": "%08x" % b[0], "name": a[2], "lib": lib, "size": b[1], "exact": exact})
    if len(sys.argv) > 8:
        json.dump(res, open(sys.argv[8], "w"), indent=1)


if __name__ == "__main__":
    main()
