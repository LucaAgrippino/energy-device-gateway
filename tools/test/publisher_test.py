#!/usr/bin/env python3
"""Publisher integration tests (components/publisher/DESIGN.md §12).

    ./publisher_test.py <esp32_ip>

Covers the rows of §12 that the system-test suite does not: multi-client
broadcast and disconnect/reconnect recovery. Handshake, JSON frame, dashboard
and latency are covered by system_test.py and latency_test.py.
"""
import sys
import time
import urllib.request

from ws_lib import WsClient

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.105"
results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}\n       {detail}\n")


# --- Dashboard ---
with urllib.request.urlopen(f"http://{HOST}/", timeout=10) as r:
    body = r.read()
record("Dashboard served at /",
       r.status == 200 and b"<" in body,
       f"HTTP {r.status}, {len(body)} bytes of HTML")

# --- Multi-client: 4 clients must see the same snapshot ---
clients = [WsClient(HOST) for _ in range(4)]
try:
    # Clients are opened sequentially, so a broadcast can land between two
    # connections and leave them one frame apart. Comparing "the Nth frame each
    # client read" therefore races. The property that actually matters is that
    # every client receives the same snapshots, so collect several from each and
    # require a common one.
    seen = []
    counts = set()
    for c in clients:
        tss = set()
        for _ in range(4):
            _, data = c.recv_json()
            tss.add(data["ts"])
            counts.add(len(data["readings"]))
        seen.append(tss)
    shared = set.intersection(*seen)
    record("Multi-client broadcast",
           len(shared) >= 2 and counts == {17},
           f"4 concurrent clients x 4 frames: {len(shared)} snapshot(s) received "
           f"identically by all four, reading counts {counts}")
finally:
    for c in clients:
        c.close()

# --- Disconnect recovery ---
c = WsClient(HOST)
before = c.recv_json()[1]["ts"]
c.close()
time.sleep(3)                      # stay away long enough to span several frames
c2 = WsClient(HOST)
t0 = time.time()
after = c2.recv_json()[1]["ts"]
resume_ms = (time.time() - t0) * 1000
n = len(c2.recv_json()[1]["readings"])
c2.close()
record("Disconnect / reconnect recovery",
       after > before and n == 17,
       f"reconnected after a 3 s absence, first frame in {resume_ms:.0f} ms, "
       f"snapshot ts advanced {before} -> {after}, {n} readings")

# --- Client churn: publisher must survive clients vanishing mid-broadcast ---
for _ in range(5):
    WsClient(HOST).close()         # connect and drop without reading
time.sleep(2)
c3 = WsClient(HOST)
ok = len(c3.recv_json()[1]["readings"]) == 17
c3.close()
record("Survives abrupt client churn",
       ok,
       "5 clients connected and dropped without reading a frame; "
       "publisher still serving a full 17-reading snapshot afterwards")

print("=" * 60)
for name, ok in results:
    print(f"  {'PASS' if ok else 'FAIL':4}  {name}")
sys.exit(0 if all(ok for _, ok in results) else 1)
