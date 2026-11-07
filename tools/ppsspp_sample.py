#!/usr/bin/env python3
"""ppsspp_sample.py PORT START_S N OUT [PERIOD_S] - a sampling profiler for a PSP program in PPSSPP (headless
--debugger=PORT): resumes the CPU (the debugger breaks at start), waits START_S seconds, then N times: pause, read
pc and ra (cpu.getAllRegs), resume, sleep PERIOD_S. Writes "pc ra" lines to OUT. The pauses slow the run down:
its PERF numbers are not timings while sampling."""
import json, sys, time, websocket
port, start, n, out = int(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
period = float(sys.argv[5]) if len(sys.argv) > 5 else 0.02
ws = None
for i in range(600):
    try: ws = websocket.create_connection("ws://127.0.0.1:%d/debugger" % port, timeout=30); break
    except Exception: time.sleep(0.5)
if not ws: sys.exit("no debugger")
def call(ev, wait=None, **kw):
    d = {"event": ev}; d.update(kw); ws.send(json.dumps(d))
    want = wait or ev
    while True:
        m = json.loads(ws.recv())
        if m.get("event") == want: return m
        if m.get("event") == "error": raise RuntimeError(m)
ws.send(json.dumps({"event": "cpu.resume"}))
t0 = time.time()
while time.time() - t0 < start: time.sleep(1); ws.settimeout(0.01)
ws.settimeout(30)
# drain
try:
    ws.settimeout(0.2)
    while True: ws.recv()
except Exception: pass
ws.settimeout(30)
f = open(out, "w")
for k in range(n):
    try:
        call("cpu.stepping", wait="cpu.stepping")
        r = call("cpu.getAllRegs")
        g = r["categories"][0]["uintValues"]
        f.write("%08x %08x\n" % (g[32], g[31]))
        ws.send(json.dumps({"event": "cpu.resume"}))
    except Exception as e:
        print("stop:", e); break
    time.sleep(period)
f.close()
print("samples", k + 1)
