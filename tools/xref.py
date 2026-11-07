"""xref.py FUNCTIONS.pkl ADDR... - functions that call (jal) or tail-call each address; the function containing it."""
import os, pickle, sys, bisect
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "PS2RC"))
f = pickle.load(open(sys.argv[1], "rb"))
starts = sorted(f)
for t in sys.argv[2:]:
    a = int(t, 16)
    i = bisect.bisect_right(starts, a) - 1
    cont = starts[i] if i >= 0 and a in f[starts[i]].addrs else None
    callers = sorted(s for s, fn in f.items() if a in fn.calls)
    tails = sorted(s for s, fn in f.items() if a in fn.tail_calls)
    print("%08x in fn %s; callers %s; tail-callers %s" % (a, "%08x" % cont if cont is not None else "-",
          " ".join("%08x" % c for c in callers[:20]) + (" ...(%d)" % len(callers) if len(callers) > 20 else ""),
          " ".join("%08x" % c for c in tails[:10])))
