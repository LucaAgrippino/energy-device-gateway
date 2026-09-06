#!/usr/bin/env python3
"""Validate /health against REQ-NF-002 and REQ-NF-003 (Day 7 plan §2).

    ./health_check.py 192.168.0.105

Exits non-zero if any budget is breached, so it can gate a release.
"""
import json
import sys
import urllib.request

HEAP_USED_LIMIT_PCT = 80.0    # REQ-NF-002
STACK_HEADROOM_MIN_PCT = 25.0  # REQ-NF-003


def main():
    host = sys.argv[1]
    with urllib.request.urlopen(f"http://{host}/health", timeout=10) as resp:
        data = json.load(resp)

    failures = []

    print(f"Uptime: {data['uptime_s']} s")
    heap = data["heap"]
    # The plan hardcodes ~320 KB here; the endpoint reports the real total, so
    # the percentage is exact rather than approximate.
    total = heap["total"]
    used_pct = (1 - heap["free"] / total) * 100
    peak_pct = (1 - heap["min_ever"] / total) * 100
    print(f"Heap: free={heap['free']} min_ever={heap['min_ever']} total={total}")
    print(f"  current {used_pct:.1f}% used, peak {peak_pct:.1f}% used "
          f"(REQ-NF-002 limit {HEAP_USED_LIMIT_PCT:.0f}%)")
    if peak_pct > HEAP_USED_LIMIT_PCT:
        failures.append(f"heap peak {peak_pct:.1f}% exceeds {HEAP_USED_LIMIT_PCT:.0f}%")

    wifi = data["wifi"]
    print(f"Wi-Fi: rssi={wifi['rssi']} dBm connected={wifi['connected']}")

    print(f"Stacks (REQ-NF-003 minimum {STACK_HEADROOM_MIN_PCT:.0f}% free):")
    for task in data["tasks"]:
        headroom = task["stack_hwm"] / task["stack_total"] * 100
        ok = headroom >= STACK_HEADROOM_MIN_PCT
        print(f"  {task['name']:<12} HWM {task['stack_hwm']:>5} / {task['stack_total']:>5} "
              f"({headroom:>5.1f}% free) [{'PASS' if ok else 'FAIL'}]")
        if not ok:
            failures.append(f"{task['name']} stack headroom {headroom:.1f}%")

    print()
    if failures:
        print("FAIL: " + "; ".join(failures))
        return 1
    print("PASS: heap and all task stacks within budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
