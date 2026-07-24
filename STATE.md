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

---

## In Progress

- [ ] Install `usbipd-win` on Windows host for USB passthrough — needs Windows-host action, not doable from WSL2
- [ ] Add `luca` to `dialout` group (`sudo usermod -aG dialout $USER` + relogin) — needs interactive sudo, not doable from this session
- [ ] Verify flash + monitor from WSL2 via USB passthrough — blocked on the two items above + board plugged in
- [ ] Create GitHub repository — needs confirmation (name/visibility/account) before creating
- [ ] Day 1: IMU component (DESIGN.md → implementation → tests)

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
| 2026-07-24 | Confirmed VISION.md already reflects WSL2-native decision (no edit needed). Completed remaining WSL2-side environment items: pymodbus installed, `.gitignore` and `.github/workflows/ci.yml` created, ESP-IDF + clangd VS Code extensions installed via `code --install-extension`. Still blocked on user/hardware action: dialout group membership, usbipd-win, flash/monitor verification, Raspberry Pi setup, GitHub repo creation. |
