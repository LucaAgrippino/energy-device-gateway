# STATE.md — Project State

**Last updated:** 2026-07-24

---

## Current Phase

**Day 1** — IMU component design and implementation.

---

## Completed

- [x] Vision document drafted (`VISION.md`)
- [x] Project instructions created (`CLAUDE.md`)
- [x] State tracking file created (`STATE.md`)
- [x] ESP32-S3-DevKitC purchased
- [x] RS-485 modules purchased (3x TTL-to-RS-485 + 1x USB-to-RS-485 CH340)
- [x] Hardware available: MPU9150 (Drotek), Raspberry Pi 3B
- [x] Dev environment decision: WSL2 native Ubuntu (no Docker for local dev)
- [x] WSL2 Ubuntu 24.04 installed and verified
- [x] Claude Code installed in WSL2
- [x] Project folder created (`~/energy-device-gateway`), git init, project docs copied
- [x] IMU decision: MPU9150 (Drotek), uses Mpu9150 driver (MPU6050-register-compatible)
- [x] I2C pull-ups: Drotek board has onboard pull-ups, no external resistors needed
- [x] Reading struct: move-only, accept memcpy escape hatch via xQueueOverwrite (POD-safe)
- [x] ESP-IDF v5.5 installed natively in WSL2 (`~/esp/esp-idf`, target esp32s3, `get_idf` alias in `.bashrc`)
- [x] Dev tools installed (clang-tidy, clang-format, picocom, cmake, ninja, dfu-util)
- [x] Verified ESP-IDF build inside WSL2 (`idf.py build` on `hello_world` for esp32s3 — succeeded)
- [x] VISION.md confirmed up to date (WSL2-native, MPU9150, no Docker for local dev — already correct, no edit needed)
- [x] pymodbus installed in WSL2 (`pip3 install pymodbus --break-system-packages`, v3.14.0)
- [x] `.gitignore` created (build/, sdkconfig, managed_components/, dependencies.lock, etc.)
- [x] `.github/workflows/ci.yml` created per VISION.md §8 spec (build/lint/test jobs; test job will only pass once `components/aggregator/host_test` exists on Day 4)
- [x] VS Code extensions installed via `code --install-extension`: `espressif.esp-idf-extension`, `llvm-vs-code-extensions.vscode-clangd` (cmake-tools and cpptools were already present)
- [x] GitHub repository created (public): https://github.com/LucaAgrippino/energy-device-gateway — local branch renamed `master`→`main` per CLAUDE.md convention, initial commit pushed (CLAUDE.md, STATE.md, VISION.md, .gitignore, .vscode/settings.json, .github/workflows/ci.yml, imu_DESIGN.md)
- [x] Confirmed `luca` already in `dialout` group (`groups luca` shows it) — no sudo action needed, prior blocker was stale
- [x] Project filesystem scaffolded per VISION.md §10 final-state tree: top-level `CMakeLists.txt`, `sdkconfig.defaults`, `.clang-tidy`, `.clang-format`, `main/` (CMakeLists.txt + minimal `app_main` stub), `components/common/include/` (`Reading.hpp`, `Snapshot.hpp` — content from VISION.md §7.3), `components/imu/DESIGN.md` moved from repo root into `components/imu/`. Empty `include/src/test` dirs created for all seven components; `wifi_manager`, `publisher`, `aggregator` (+`host_test`), `modbus_device`, `health` intentionally left without `DESIGN.md`/`CMakeLists.txt` since they haven't been designed yet. `tools/pymodbus_slave/`, `dashboard/`, `docs/` created empty for later days.
- [x] Verified scaffold builds clean: `idf.py set-target esp32s3 && idf.py build` succeeds (empty component dirs correctly skipped, `main.cpp` compiles, `.bin` generated — 80% app partition free)
- [x] `usbipd attach --wsl` verified working (previous elevated-PowerShell issue confirmed fixed — ran non-elevated per VISION.md §2.4 note, no error). Board enumerates in WSL2 as `/dev/ttyACM0` (built-in USB JTAG/CH343, VID:PID `1a86:55d3`).
- [x] Flash + monitor verified end-to-end: `idf.py -p /dev/ttyACM0 flash` succeeded; serial boot log confirmed via direct read of `/dev/ttyACM0` (bootloader → app_main → `"energy-device-gateway starting"` log line). `idf.py monitor` itself can't run non-interactively from this session (needs a real TTY for keypresses), so monitoring was verified with a raw serial read instead — works fine interactively from a normal terminal.
- [x] `sdkconfig.defaults` flash size corrected: added `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` / `CONFIG_ESPTOOLPY_FLASHSIZE="16MB"` to match the board's actual 16MB chip. Regenerated `sdkconfig` (gitignored, deleted and rebuilt) and reflashed — boot log's "Detected size(16384k) larger than..." warning is gone.
- [x] CLAUDE.md's Architecture Summary corrected: `IImu ← Mpu6050, Mpu9250` → `IImu ← Mpu9150 (MPU6050-register-compatible)`, matching the resolved IMU decision and DESIGN.md.

---

## In Progress

- [ ] Day 1: IMU component implementation (`IImu.hpp`, `Mpu9150.hpp`/`.cpp`, `CMakeLists.txt`, `ImuTask`, unit tests) — DESIGN.md complete, ready to start

---

## Open Items (not blockers, deferred by design)

- [ ] `components/common/include/DeviceConfig.hpp` — listed in VISION.md §10 tree but its fields are never specified anywhere; content deferred until a component that needs it (likely wifi_manager or modbus_device) is designed
- [ ] `main/Kconfig.projbuild`, `README.md`, `ARCHITECTURE.md`, `docs/ram_budget.md` — listed in VISION.md §10 as final-state files but are Day 2/Day 7 deliverables with no content to write yet

---

## Not Started

- [ ] Set up Raspberry Pi with pymodbus (separate hardware)
- [ ] Day 2: Wi-Fi Manager
- [ ] Day 3: WebSocket Publisher
- [ ] Day 4: Aggregator
- [ ] Day 5: Modbus RTU
- [ ] Day 6: Modbus TCP
- [ ] Day 7: System test + acceptance + documentation

---

## Resolved Decisions

1. **Reading struct: move-only or copyable?** → Move-only. Accept `memcpy` escape
   hatch via `xQueueOverwrite` since `Reading` is POD-safe (string_view + float +
   int64_t + enum). The aggregator always produces fresh snapshots.

2. **Which IMU for initial development?** → MPU9150 (Drotek). Uses `Mpu9150` driver
   class (MPU6050-register-compatible for accel/gyro). Default I2C address `0x69`
   (Drotek AD0 pulled high). WHO_AM_I returns `0x68`.

3. **I2C pull-up resistors?** → Not needed. Drotek MPU9150 board has onboard pull-ups.

4. **Reading move-only vs xQueueOverwrite?** → Option (a). Keep move-only semantics,
   accept `memcpy` via `xQueueOverwrite` as an escape hatch. POD-safe for the current
   struct layout.

---

## Open Decisions

None currently.

---

## Blockers

None currently.

---

## Session Log

| Date | Summary |
|------|---------|
| 2026-07-23 | Vision document created. Dev environment plan established (Docker Dev Container + usbipd-win). BOM finalised. CLAUDE.md and STATE.md created. |
| 2026-07-24 | Dev environment changed from Docker Dev Container to WSL2 native Ubuntu. VISION.md, CLAUDE.md, STATE.md updated. Removed `.devcontainer/` from repo structure, added `.vscode/settings.json`. |
| 2026-07-24 | WSL2 verified, Claude Code installed. IMU decided: MPU9150 (Drotek) with Mpu6050 driver. All open decisions resolved. BOM and class hierarchy updated. IMU DESIGN.md started. |
| 2026-07-24 | ESP-IDF v5.5 installed natively in WSL2 (esp32s3 target), dev tools (clang-tidy, clang-format, picocom, cmake, ninja) installed via apt, toolchain verified with a successful `hello_world` build. `.vscode/settings.json` created with IDF paths. Remaining env items (usbipd-win, VS Code extension install, RPi setup, flash/monitor verification) need Windows-host or hardware action. |
| 2026-07-24 | Confirmed VISION.md already reflects WSL2-native decision (no edit needed). Completed remaining WSL2-side environment items: pymodbus installed, `.gitignore` and `.github/workflows/ci.yml` created, ESP-IDF + clangd VS Code extensions installed via `code --install-extension`. Created public GitHub repo, renamed branch to `main`, pushed initial commit. CI workflow triggers automatically but will fail until Day 1 firmware skeleton (CMakeLists.txt, main/, components/) exists — expected, not a setup bug. Still blocked on user/hardware action: dialout group membership, usbipd-win, flash/monitor verification, Raspberry Pi setup. |
| 2026-07-24 | Fixed invalid `permissions.defaultMode` in `~/.claude/settings.json` (was `"allowEdits"`, an invalid value — set to `"acceptEdits"`). Confirmed `dialout` group membership was already satisfied — that STATE.md blocker was stale. Scaffolded full project filesystem per VISION.md §10: top-level build files, `main/` stub, `components/common` shared types, all seven component directories (only `imu` and `common` populated — others await their design day), `tools/pymodbus_slave/`, `dashboard/`, `docs/`. Verified with a clean `idf.py build`. USB passthrough (`usbipd-win`) still stuck on the Windows-host side, so flash/monitor verification remains blocked; next unblocked step is IMU component implementation against the existing DESIGN.md. |
| 2026-07-25 | Environment setup finished: with the board plugged into the Windows host, `usbipd attach --wsl` ran clean from a non-elevated PowerShell (the #1008 issue from last session did not recur) — board enumerates in WSL2 as `/dev/ttyACM0`. Flashed the Day 1 scaffold and confirmed the full boot log over serial, including the `app_main` log line, verifying flash + monitor end-to-end. Noted (not fixed): `sdkconfig.defaults` flash size (2MB) doesn't match the board's actual 16MB chip. Day 1 scaffolding (build files, `main/`, `components/common`, `DESIGN.md` move) remains uncommitted, staged for a decision on when to commit. |
