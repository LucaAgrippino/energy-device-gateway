# Day 7 — System Test, Acceptance & Documentation

**Project:** `energy-device-gateway`
**Author:** Luca Agrippino
**Date:** 2026-07-26
**Status:** Plan

---

## 1. Overview

Day 7 is the right side of the V-model: verifying that the assembled system meets
all requirements. Three activities:

1. **System Test** — full pipeline end-to-end with all hardware
2. **Acceptance Test** — walk every REQ-F-xxx and REQ-NF-xxx, pass/fail
3. **Documentation** — README.md, ARCHITECTURE.md, RAM budget

---

## 2. Test Setup

### Hardware Configuration

```
                   Wi-Fi (STA mode)
ESP32-S3 ←─────────────────────────────────→ Router ←──→ Laptop (browser)
  │
  ├── GPIO 1,2 (I2C) ──→ Drotek MPU9150
  │
  ├── GPIO 17,18,8 (UART/RS-485) ──→ RS-485 Module ──→ USB/RS-485 Adapter ──→ RPi
  │                                                                      (rtu_slave.py)
  └── Wi-Fi (TCP) ─────────────────────────────────────────────────────→ RPi
                                                                      (tcp_slave.py)
```

### Software Running

| Device | Software | Command |
|--------|----------|---------|
| Raspberry Pi | pymodbus RTU slave | `python3 tools/pymodbus_slave/rtu_slave.py` |
| Raspberry Pi | pymodbus TCP slave | `python3 tools/pymodbus_slave/tcp_slave.py` |
| ESP32-S3 | Firmware | `idf.py -p /dev/ttyACM0 flash monitor` |
| Laptop | Browser | Navigate to `http://<ESP32_IP>/` |
| Laptop | WebSocket client | `python3 tools/test/ws_client.py` |

### Test Tools (in `tools/test/`)

```python
# tools/test/ws_client.py — WebSocket test client
import websocket, json, time, sys

ws = websocket.create_connection(f"ws://{sys.argv[1]}/ws")
for i in range(10):
    msg = ws.recv()
    data = json.loads(msg)
    print(f"Frame {i}: ts={data['ts']}, readings={len(data['readings'])}")
    for r in data['readings']:
        print(f"  {r['src']:25s} = {r['val']:10.3f}  status={r['st']}")
ws.close()
```

```python
# tools/test/health_check.py — Health endpoint validator
import requests, json, sys

resp = requests.get(f"http://{sys.argv[1]}/health")
data = resp.json()

print(f"Uptime: {data['uptime_s']}s")
print(f"Heap: free={data['heap']['free']}, min_ever={data['heap']['min_ever']}")
print(f"Wi-Fi: RSSI={data['wifi']['rssi']}, connected={data['wifi']['connected']}")

total_heap = 320 * 1024  # Approximate total DRAM
heap_used_pct = (1 - data['heap']['free'] / total_heap) * 100
print(f"Heap usage: {heap_used_pct:.0f}% (target: ≤80%)")

for task in data['tasks']:
    headroom = task['stack_hwm'] / task['stack_total'] * 100
    status = "PASS" if headroom >= 25 else "FAIL"
    print(f"  {task['name']:15s}: HWM {task['stack_hwm']:5d} / {task['stack_total']:5d} "
          f"({headroom:.0f}% free) [{status}]")
```

```python
# tools/test/latency_test.py — WebSocket latency measurement
import websocket, json, time, sys

ws = websocket.create_connection(f"ws://{sys.argv[1]}/ws")
latencies = []
for i in range(20):
    t0 = time.monotonic()
    msg = ws.recv()
    t1 = time.monotonic()
    data = json.loads(msg)
    # Approximate: time between consecutive frames
    if i > 0:
        latencies.append((t1 - t0) * 1000)

ws.close()
avg = sum(latencies) / len(latencies)
mx = max(latencies)
print(f"Frame interval: avg={avg:.0f}ms, max={mx:.0f}ms (target: ≤500ms)")
```

---

## 3. System Tests

### ST-001: Full Pipeline — IMU to Browser

**Preconditions:** MPU9150 wired, Wi-Fi connected, firmware running.

**Steps:**
1. Open browser at `http://<ESP32_IP>/`
2. Open WebSocket client: `python3 ws_client.py <ESP32_IP>`
3. Observe JSON frames arriving every ~500 ms

**Expected:**
- JSON contains `imu.accel_x`, `imu.accel_y`, `imu.accel_z`, `imu.gyro_x`,
  `imu.gyro_y`, `imu.gyro_z` readings
- `accel_z` ≈ 9.81 m/s² (gravity, device stationary on flat surface)
- All IMU readings have `status: 0` (OK)

---

### ST-002: Full Pipeline — Modbus RTU to Browser

**Preconditions:** RPi running `rtu_slave.py`, RS-485 wired.

**Steps:**
1. Start `rtu_slave.py` on RPi
2. Open WebSocket client
3. Observe JSON frames

**Expected:**
- JSON contains `modbus_rtu.voltage` ≈ 48.5V, `modbus_rtu.current` ≈ 10.5A,
  `modbus_rtu.power`, `modbus_rtu.energy_total`, `modbus_rtu.status`
- All RTU readings have `status: 0` (OK)
- Values change slightly each second (simulated)

---

### ST-003: Full Pipeline — Modbus TCP to Browser

**Preconditions:** RPi running `tcp_slave.py`, Wi-Fi connected, same network.

**Steps:**
1. Start `tcp_slave.py` on RPi
2. Open WebSocket client
3. Observe JSON frames

**Expected:**
- JSON contains `modbus_tcp.voltage`, `modbus_tcp.current`, etc.
- All TCP readings have `status: 0` (OK)

---

### ST-004: All Sources Combined

**Preconditions:** IMU wired, both pymodbus slaves running.

**Steps:**
1. Open WebSocket client
2. Verify all three source groups appear in a single JSON frame

**Expected:**
- One frame contains IMU (6 readings) + RTU (5 readings) + TCP (5 readings)
- Total ~16 readings per frame
- All `status: 0`

---

### ST-005: AP Mode Fallback

**Preconditions:** Erase NVS or provide invalid Wi-Fi credentials.

**Steps:**
1. Flash with invalid SSID in `sdkconfig.defaults`
2. Monitor serial output
3. Wait for timeout (10s default)

**Expected:**
- Serial shows STA connection attempts
- After timeout, serial shows "Starting AP mode" with SSID `EDG-Setup`
- Laptop can see `EDG-Setup` network

---

### ST-006: Wi-Fi Reconnection

**Preconditions:** Firmware running, Wi-Fi connected, WebSocket streaming.

**Steps:**
1. Verify WebSocket frames arriving
2. Disable router Wi-Fi (or disconnect ESP32 from network)
3. Wait 10 seconds
4. Re-enable router Wi-Fi

**Expected:**
- WebSocket connection drops when Wi-Fi goes down
- Serial shows `WIFI_EVENT_STA_DISCONNECTED`, reconnect attempts
- Wi-Fi reconnects automatically (no reboot)
- New WebSocket connection from browser works
- Modbus TCP readings show `status: 1` (TIMEOUT) during outage, recover after

---

### ST-007: Stale Detection

**Preconditions:** All sources initially working.

**Steps:**
1. Verify all readings OK
2. Stop `rtu_slave.py` on RPi (kill the process)
3. Wait 5 seconds
4. Check WebSocket frames

**Expected:**
- RTU readings change to `status: 1` (TIMEOUT) after 3 seconds
- IMU readings remain `status: 0` (OK)
- TCP readings remain `status: 0` (OK)
- System continues operating — no crash, no freeze

---

### ST-008: Source Independence (REQ-NF-001)

**Preconditions:** IMU wired, NO Modbus slaves running.

**Steps:**
1. Flash and run firmware without starting any pymodbus slave
2. Open WebSocket client

**Expected:**
- IMU readings arrive with `status: 0` (OK)
- Modbus RTU and TCP readings show `status: 1` (TIMEOUT) or `status: 2` (ERROR)
- Aggregator and publisher keep running
- No crash, no hang, no blocked task

---

## 4. Acceptance Test Checklist

### Functional Requirements

| ID | Requirement | Test Method | Status |
|----|-------------|-------------|--------|
| REQ-F-001 | Read accel + gyro from IMU over I2C | ST-001: verify accel_z ≈ 9.81 | ☐ |
| REQ-F-002 | Support MPU6050-compatible IMUs via IImu | Code review: Mpu9150 implements IImu | ☐ |
| REQ-F-003 | Connect Wi-Fi STA with NVS credentials | ST-001: verify IP assigned, monitor shows connected | ☐ |
| REQ-F-004 | Fall back to AP mode on STA failure | ST-005: verify AP mode after timeout | ☐ |
| REQ-F-005 | WebSocket endpoint at /ws streams JSON | ST-001: ws_client.py receives valid JSON | ☐ |
| REQ-F-006 | Read Modbus RTU holding registers over RS-485 | ST-002: verify scaled register values | ☐ |
| REQ-F-007 | Read Modbus TCP holding registers over Wi-Fi | ST-003: verify scaled register values | ☐ |
| REQ-F-008 | Aggregate readings into periodic snapshots | ST-004: all sources in one JSON frame | ☐ |
| REQ-F-009 | Detect stale readings and mark TIMEOUT | ST-007: stop slave, verify TIMEOUT status | ☐ |
| REQ-F-010 | Serve HTML dashboard at / | Browser: navigate to /, verify page loads | ☐ |
| REQ-F-011 | /health endpoint with heap, RSSI, HWM | health_check.py: verify all fields present | ☐ |
| REQ-F-012 | Persist Wi-Fi credentials via NVS | Reboot ESP32, verify auto-reconnect without re-entering SSID | ☐ |

### Non-Functional Requirements

| ID | Requirement | Test Method | Status |
|----|-------------|-------------|--------|
| REQ-NF-001 | Unresponsive source doesn't block others | ST-008: no slaves running, IMU still works | ☐ |
| REQ-NF-002 | ≤80% heap at steady state | health_check.py: verify free heap > 20% of ~320 KB | ☐ |
| REQ-NF-003 | ≥25% stack headroom per task | health_check.py: verify all tasks ≥25% | ☐ |
| REQ-NF-004 | WebSocket latency ≤500 ms | latency_test.py: verify frame interval ≤500 ms | ☐ |
| REQ-NF-005 | Recover from Wi-Fi disconnect without reboot | ST-006: disable/re-enable Wi-Fi, verify recovery | ☐ |
| REQ-NF-006 | RAII for all dynamic resources | Code review: every constructor/destructor pair | ☐ |
| REQ-NF-007 | Zero warnings under -Wall -Wextra | CI: `idf.py build` output + clang-tidy | ☐ |
| REQ-NF-008 | All dev and CI on Linux | CI: GitHub Actions with espressif/idf:v5.5 | ☐ |

---

## 5. Documentation Deliverables

### README.md

```markdown
# Energy Device Gateway

ESP32-S3 firmware that collects data from local sensors (IMU over I2C) and
industrial devices (Modbus RTU over RS-485, Modbus TCP over Wi-Fi), aggregates
telemetry, and streams it in real time over WebSocket to a browser dashboard.

## Quick Start

### Prerequisites
- ESP-IDF v5.5 (installed in WSL2 Ubuntu 24.04)
- ESP32-S3-DevKitC
- Drotek MPU9150 (I2C)
- RS-485 module + USB/RS-485 adapter
- Raspberry Pi 3B with pymodbus

### Build & Flash
\```bash
get_idf
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
\```

### Configuration
\```bash
idf.py menuconfig
# → Wi-Fi Manager Configuration (SSID, password)
# → Modbus RTU Configuration (UART, pins, baud rate)
# → Modbus TCP Configuration (IP, port)
\```

## Architecture
See [ARCHITECTURE.md](ARCHITECTURE.md) for system design, data flow, and
component map.

## Development
See [VISION.md](VISION.md) for requirements, V-model process, and development plan.
See [CLAUDE.md](CLAUDE.md) for coding standards and project rules.

## License
MIT
```

### ARCHITECTURE.md

Extracted from VISION.md §6, cleaned up for public consumption:
- System context diagram (SVG)
- Task model table
- Data flow diagram
- Component map
- Class hierarchy diagrams
- Key design decisions (FreeRTOS vs C++ concurrency split)

### docs/ram_budget.md

| Category | Estimated | Measured | Notes |
|----------|-----------|----------|-------|
| Wi-Fi driver | 60–70 KB | TBD | Largest consumer |
| Task stacks (7 tasks) | ~30 KB | TBD | 4096 × 5 + 6144 × 1 + 4096 × 1 |
| FreeRTOS kernel | ~10 KB | TBD | Scheduler, idle task, timer task |
| Application data | ~5 KB | TBD | Snapshots, readings, JSON buffers |
| **Total used** | **~105–115 KB** | **TBD** | |
| **Total DRAM** | **~320 KB** | | |
| **Free** | **~205–215 KB** | **TBD** | Target: ≥ 64 KB (≥20%) |

Fill the "Measured" column from the `/health` endpoint after the system is running.

---

## 6. Day 7 Checklist

| # | Task | Status |
|---|------|--------|
| 1 | Start pymodbus RTU slave on RPi | ☐ |
| 2 | Start pymodbus TCP slave on RPi | ☐ |
| 3 | Flash firmware to ESP32-S3 | ☐ |
| 4 | Run ST-001 through ST-008 | ☐ |
| 5 | Run acceptance checklist (all REQ-F, REQ-NF) | ☐ |
| 6 | Run `health_check.py`, record heap and stack numbers | ☐ |
| 7 | Run `latency_test.py`, record latency numbers | ☐ |
| 8 | Fill measured values in `docs/ram_budget.md` | ☐ |
| 9 | Run `idf.py size-components`, document in ARCHITECTURE.md | ☐ |
| 10 | Write README.md | ☐ |
| 11 | Write ARCHITECTURE.md | ☐ |
| 12 | Final commit: `docs: add README, ARCHITECTURE, RAM budget` | ☐ |
| 13 | Tag: `v1.0` | ☐ |

---

## 7. Definition of Done — Project Complete

The project is complete when:

- [ ] All 12 functional requirements pass acceptance
- [ ] All 8 non-functional requirements pass acceptance
- [ ] CI pipeline green (build + lint + host test)
- [ ] README.md, ARCHITECTURE.md, ram_budget.md written
- [ ] All DESIGN.md files complete with measured stack/heap values
- [ ] Repository tagged `v1.0`
- [ ] Demo: open browser dashboard, show live telemetry from all three sources
