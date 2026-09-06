# Acceptance Test Results

Checklist from [day7_system_test_plan.md](day7_system_test_plan.md) §4, walked
against the running system.

**Date:** 2026-09-06
**Firmware:** `main` @ `97610e4`, ESP-IDF v5.5.5
**Rig:** ESP32-S3-DevKitC at `192.168.0.105`; Raspberry Pi 3B at `192.168.0.209`
running `tcp_slave.py` and `rtu_slave.py`; both on the same 2.4 GHz BSS. RS-485 bus
wired ESP32 ↔ CH340 adapter over Cat6.

**Result: 20 of 20 requirements PASS, 0 BLOCKED, 0 FAIL.**

Two defects were found and fixed in the course of verifying them (REQ-NF-001 and
REQ-NF-005); both are re-verified above.

---

## Functional requirements

| ID | Requirement | Evidence | Status |
|---|---|---|---|
| REQ-F-001 | Read accel + gyro from IMU over I2C | ST-001: 7 IMU readings, all `OK`, ‖accel‖ = 9.87 m/s² | **PASS** |
| REQ-F-002 | MPU6050-compatible IMUs via `IImu` | `Mpu9150 : IImu`; on-target WHO_AM_I probe and scaling tests pass | **PASS** |
| REQ-F-003 | Wi-Fi STA using NVS credentials | Board joined `VM0898060` in 2.9 s, took `192.168.0.105` | **PASS** |
| REQ-F-004 | AP fallback on STA failure | ST-005: 5 retries → `falling back to AP mode` → `EDG-Setup` visible and joinable | **PASS** |
| REQ-F-005 | `/ws` streams JSON telemetry | ST-001/003/004: valid JSON, 17 readings, every 500 ms. `publisher_test.py`: 4 concurrent clients receive the identical snapshot; reconnect after a 3 s absence resumes in 334 ms; publisher survives 5 clients dropping mid-broadcast | **PASS** |
| REQ-F-006 | Modbus RTU holding registers over RS-485 | ST-002: decoded `V=365.400 I=0.700 P=249.000 status=1` against the simulator's `V=365.4 I=0.70 P=249 W status=1` at 18:58:07 — exact match over real RS-485 | **PASS** |
| REQ-F-007 | Modbus TCP holding registers over Wi-Fi | ST-003: decoded values identical to the simulator's ground truth at the same instant | **PASS** |
| REQ-F-008 | Aggregate readings into periodic snapshots | ST-004: one frame carries imu 7 + rtu 5 + tcp 5 = 17 | **PASS** |
| REQ-F-009 | Detect stale readings, mark TIMEOUT | Host tests cover stale → `TIMEOUT` and empty mailbox → `TIMEOUT`; ST-007 covers the live failure path | **PASS** |
| REQ-F-010 | Serve HTML dashboard at `/` | `GET /` → `200`, 2087 bytes of HTML | **PASS** |
| REQ-F-011 | `/health` with heap, RSSI, stack HWM | `health_check.py` parses all documented fields | **PASS** |
| REQ-F-012 | Persist Wi-Fi credentials via NVS | Reflashed ~8× this session; the board rejoined each time without reprovisioning | **PASS** |

## Non-functional requirements

| ID | Requirement | Evidence | Status |
|---|---|---|---|
| REQ-NF-001 | Unresponsive source doesn't block others | ST-008: TCP slave killed while RS-485 kept streaming `OK`, then recovered. Also **fixed a violation**: `ESP_ERROR_CHECK(g_imu->init())` reboot-looped the whole gateway on a loose I2C wire; the IMU now degrades to `TIMEOUT` like the Modbus sources | **PASS** |
| REQ-NF-002 | ≤ 80% heap at steady state | Peak 35.9% used (221,300 free of 345,436) | **PASS** |
| REQ-NF-003 | ≥ 25% stack headroom per task | Worst is `modbus_rtu` at 42.1%; all six tasks pass | **PASS** |
| REQ-NF-004 | WebSocket latency ≤ 500 ms | median 55 ms, p95 275 ms, max 413 ms | **PASS** |
| REQ-NF-005 | Recover from Wi-Fi disconnect without reboot | ST-006: 45 s AP outage → retries → AP fallback → AP-mode retry timer → reconnected, no reset. **Found and fixed a real defect**: `AP_MODE` was terminal, so any outage beyond ~12 s stranded the gateway until a human intervened | **PASS** |
| REQ-NF-006 | RAII for all dynamic resources | Every I2C/UART/socket/server/Wi-Fi handle is constructor-acquired and destructor-released; no raw `new`/`delete`; components held by `unique_ptr` | **PASS** |
| REQ-NF-007 | Zero warnings under `-Wall -Wextra` | Clean build, zero warnings; clang-tidy exits 0 | **PASS** |
| REQ-NF-008 | All dev and CI on Linux | Ubuntu 24.04 native; CI on `ubuntu-latest` with `espressif/idf:v5.5` | **PASS** |

---

## How REQ-NF-005 was tested

The router could not be power-cycled and the board could not be moved out of
range, so a controlled AP was built instead: the Pi was put on `VM0898060` and
made to route the notebook's traffic over the existing Ethernet link, which
freed the notebook's radio to serve a 2.4 GHz AP the board joined. The AP could
then be dropped and restored on command with the serial log captured throughout,
and the notebook stayed online the whole time.

That rig is what exposed the defect. A 25 s outage left the board in
provisioning AP mode permanently — it never noticed the AP return, and only a
reset recovered it, which is exactly what REQ-NF-005 forbids. After the fix, a
**45 s** outage recovers on its own with no reset.

## Standing hardware note — IMU ground

The Freenove ESP32-S3 WROOM board exposes a single GND pin, which the RS-485
module occupies. With the IMU's ground floating it is parasitically powered
through SDA/SCL: it still ACKs its address at `0x69` on every scan, and register
*writes* succeed, but *reads* fail — enough to look present while returning
nothing. ST-001 and ST-007 therefore fail whenever that ground is not connected.
Both pass with it connected, which is how REQ-F-001 was verified. A ground
junction (the RS-485 module's second GND, or a breadboard rail) resolves it
permanently.

---

## Definition of done (plan §7)

- [x] All functional requirements pass acceptance — **12 of 12**
- [x] All non-functional requirements pass acceptance — **8 of 8**
- [x] CI pipeline green — run 34045246740: build, lint and test all pass
- [x] README.md, ARCHITECTURE.md, ram_budget.md written
- [x] All DESIGN.md files complete, with measured stack/heap values recorded in `docs/ram_budget.md`
- [x] Repository tagged `v1.0`
- [x] Demo: browser dashboard shows live telemetry from **all three** sources, 17 readings all `OK`
