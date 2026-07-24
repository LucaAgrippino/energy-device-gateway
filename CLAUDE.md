# CLAUDE.md — Project Instructions

This file provides context and rules for any AI assistant (Claude chat or Claude Code)
working on the `energy-device-gateway` project.

---

## Project Overview

ESP32-S3 gateway firmware that collects data from local sensors (IMU over I2C) and
industrial devices (Modbus RTU over RS-485, Modbus TCP over Wi-Fi), aggregates telemetry,
and streams it in real time over WebSocket to a browser dashboard.

- **Target:** ESP32-S3-DevKitC
- **Framework:** ESP-IDF v5.5, C++
- **OS:** Linux (Ubuntu 24.04 via WSL2, native installation — no Docker for local dev)
- **Owner:** Luca Agrippino
- **Repository:** `energy-device-gateway`

---

## Architecture Summary

See `VISION.md` for full details. Key points:

- **Components** (under `components/`): `imu`, `modbus_device`, `aggregator`, `publisher`,
  `wifi_manager`, `health`, `common`.
- **Data flow:** Sensor tasks → `xQueueOverwrite` (mailbox) → `AggregatorTask` →
  `std::condition_variable` → `PublisherTask` → WebSocket.
- **Class hierarchy:** `IImu` ← `Mpu6050`, `Mpu9250`. `IModbusDevice` ← `ModbusRtuDevice`,
  `ModbusTcpDevice`.
- **Concurrency split:** FreeRTOS primitives (`xQueueOverwrite`) for sensor→aggregator
  path (scheduler-integrated blocking with timeout, mailbox semantics). C++
  `std::mutex`/`std::condition_variable` for aggregator→publisher path (demonstrates
  C++ concurrency as Patrick requested).

---

## Development Process

### V-Model

- Every module gets a `DESIGN.md` **before** implementation.
- Tests at four levels: Unit (per-module, Unity), Integration (cross-module, on-target),
  System (end-to-end, Day 7), Acceptance (requirements checklist).
- One PR per component. Conventional commits.
- Tag each completed module: `<module>-v1.0`.

### Branch Strategy

- `main` — stable, CI-green.
- `feature/<module-name>` — development branches.

### CI

- GitHub Actions with `espressif/idf:v5.5` Docker image (CI only — local dev uses native WSL2).
- Pipeline: Build → Lint (clang-tidy) → Test (host-based Unity) → Size report.

---

## Coding Standards

### C++ Style

- **RAII mandatory** for all resource acquisition (I2C handles, UART ports, Modbus masters,
  Wi-Fi init/deinit). Constructor acquires, destructor releases.
- **Smart pointers:** `std::unique_ptr` for ownership, no raw `new`/`delete`.
- **Move semantics:** `Reading` struct is move-only (deleted copy constructor/assignment).
- **`std::string_view`:** for names and identifiers — zero allocation from string literals.
- **`std::vector`:** for collections of readings in snapshots.
- **`extern "C" void app_main(void)`** in `main.cpp`.

### ESP-IDF Conventions

- Use ESP-IDF logging: `ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE` with a `TAG` per file.
- Error handling: check `esp_err_t` returns, use `ESP_ERROR_CHECK` for fatal init errors,
  graceful handling for runtime errors.
- FreeRTOS task stack depth is in **bytes** on ESP32 (unlike the POSIX simulator where it's
  in words on some ports). Validate with `uxTaskGetStackHighWaterMark`, maintain ≥25% headroom.
- `sdkconfig.defaults` is tracked in git; `sdkconfig` is in `.gitignore`.

### Commit Messages

Follow conventional commits:
```
feat(imu): add MPU6050 I2C driver with RAII
fix(aggregator): handle stale reading timeout correctly
test(imu): add WHO_AM_I register probe test
refactor(common): make Reading move-only
docs(imu): add DESIGN.md with data flow diagram
chore: add .vscode settings for ESP-IDF
```

---

## File Structure Rules

- Each component lives under `components/<name>/` with `CMakeLists.txt`, `DESIGN.md`,
  `include/`, `src/`, and `test/` subdirectories.
- Shared types live in `components/common/include/`.
- `main/` contains only `app_main` — task creation and wiring, no business logic.
- Mermaid diagrams go inline in markdown files (rendered by GitHub).
- pymodbus slave scripts live in `tools/pymodbus_slave/`.
- Browser dashboard HTML lives in `dashboard/`.
- VS Code workspace settings live in `.vscode/settings.json`.

---

## State Tracking

See `STATE.md` for current project state: what's done, what's in progress,
open decisions, and blockers. Update this file at the end of each working session.

---

## Interaction Style

- **Socratic guidance preferred** — guide Luca to solutions through questions rather than
  handing answers directly, especially for design decisions.
- **Single focused questions** — avoid multi-part compound questions.
- **Design before code** — always write/review `DESIGN.md` before implementing.
- **Ground advice in specs** — reference ESP-IDF docs, FreeRTOS API, or datasheets.
  Don't make claims that aren't backed by actual documentation.
- **Concise and relevant** — no fluff, no contradictions.
