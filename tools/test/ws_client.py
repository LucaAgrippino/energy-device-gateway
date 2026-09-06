#!/usr/bin/env python3
"""Print telemetry frames from the gateway (Day 7 plan §2, ST-001..ST-004).

    ./ws_client.py 192.168.0.105 [frame_count]
"""
import sys

from ws_lib import WsClient

STATUS = {0: "OK", 1: "TIMEOUT", 2: "ERROR"}


def main():
    host = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 10

    with WsClient(host) as ws:
        for i in range(count):
            _, data = ws.recv_json()
            print(f"Frame {i}: ts={data['ts']}  readings={len(data['readings'])}")
            for r in data["readings"]:
                print(f"  {r['src']:<25} = {r['val']:>10.3f}  "
                      f"status={r['st']} ({STATUS.get(r['st'], '?')})")
            print()


if __name__ == "__main__":
    main()
