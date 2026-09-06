#!/usr/bin/env python3
"""Measure snapshot-to-browser latency against REQ-NF-004 (<= 500 ms).

    ./latency_test.py 192.168.0.105 [seconds]

The plan's snippet times the gap between consecutive frames, which measures the
aggregator's 500 ms period rather than latency — it would report ~500 ms however
slow delivery actually was. This anchors the board clock to the host instead.

The board's log prefix cannot be used as that anchor: with
CONFIG_LOG_TIMESTAMP_SOURCE_RTOS the prefix comes from the FreeRTOS tick, which
was measured running a constant 355 ms ahead of esp_timer_get_time() — the clock
the frame's `ts` field uses. Anchoring on it inflates every figure by that much.
Instead the offset is estimated from the frames themselves: over many samples the
smallest (arrival - ts) is the one that waited least in transit, so it bounds the
true clock offset, and every other frame's latency is measured relative to it.
This yields latency *above the best observed frame*, which is a lower bound on
absolute latency and the right quantity for judging the tail.
"""
import statistics
import sys
import time

from ws_lib import WsClient

LIMIT_MS = 500.0


def main():
    host = sys.argv[1]
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0

    samples = []
    deadline = time.time() + duration
    with WsClient(host, timeout=duration + 10) as ws:
        while time.time() < deadline:
            arrival, data = ws.recv_json()
            samples.append((arrival, data["ts"] / 1e6))

    if len(samples) < 5:
        print(f"too few frames ({len(samples)})")
        return 1

    offset = min(a - t for a, t in samples)
    lat = sorted((a - t - offset) * 1000 for a, t in samples)
    board_gaps = [(samples[i + 1][1] - samples[i][1]) * 1000 for i in range(len(samples) - 1)]

    print(f"frames: {len(samples)}")
    print(f"board snapshot cadence: median {statistics.median(board_gaps):.1f} ms "
          f"(stdev {statistics.pstdev(board_gaps):.1f})")
    print()
    print(f"Latency above best observed frame (REQ-NF-004 limit {LIMIT_MS:.0f} ms)")
    print(f"  median {statistics.median(lat):7.1f} ms")
    print(f"  mean   {statistics.mean(lat):7.1f} ms")
    print(f"  p95    {lat[max(0, int(len(lat) * 0.95) - 1)]:7.1f} ms")
    print(f"  max    {max(lat):7.1f} ms")
    print()
    ok = max(lat) <= LIMIT_MS
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
