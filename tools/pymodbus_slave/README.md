# pymodbus Slave Simulators

Modbus slaves that stand in for a solar inverter, so the ESP32 gateway has
something to talk to. They run on the Raspberry Pi 3B (`rpi-byte`).

- `inverter.py` — shared datastore and simulation, no transport
- `rtu_slave.py` — Modbus RTU over RS-485, via the USB-to-RS-485 (CH340) adapter
- `tcp_slave.py` — Modbus TCP (Day 6, not written yet)

## Register map

Fixed by VISION.md §7.2. The ESP32 side is `kInverterRegisters` in `main/main.cpp`;
both must agree or readings silently decode to nonsense.

| Register | Name          | Type        | Unit | Scale |
|----------|---------------|-------------|------|-------|
| 0        | DC Voltage    | uint16      | V    | ×0.1  |
| 1        | DC Current    | uint16      | A    | ×0.01 |
| 2        | AC Power      | uint16      | W    | ×1    |
| 3–4      | Energy Total  | uint32 (BE) | kWh  | ×0.1  |
| 5        | Device Status | uint16      | —    | 0=off, 1=running, 2=fault |

All in the holding-register bank (function code `0x03`).

## Wire settings

Must match `components/modbus_device/Kconfig`:

| Setting | Value |
|---------|-------|
| Port | `/dev/ttyUSB0` |
| Baud | 9600 |
| Framing | 8N1 |
| Slave address | 1 |

## Install

```bash
python3 -m venv ~/pymodbus-venv
~/pymodbus-venv/bin/pip install -r requirements.txt
```

**pymodbus must be from the 3.8 line.** 3.9 renamed `ModbusSlaveContext` to
`ModbusDeviceContext` and reduced it to a write-once shim with no `setValues()`,
so the periodic updates in `inverter.py` become impossible — and the modern
`SimData`/`SimDevice` replacement exposes no public runtime-mutation API either.
Debian trixie's `python3-pymodbus` is 3.8.6 and also works.

## Run

```bash
~/pymodbus-venv/bin/python rtu_slave.py            # defaults
~/pymodbus-venv/bin/python rtu_slave.py --verbose  # log every frame
~/pymodbus-venv/bin/python rtu_slave.py --fault    # pin status to 2
```

`--verbose` is what you want while debugging the bus: it prints each decoded
request and response.

## Gotchas

- **The port check is deliberate.** `StartAsyncSerialServer` only logs a warning
  if the serial port is missing, then keeps running — the simulation loop carries
  on printing plausible values while serving nothing at all. `rtu_slave.py`
  pre-flights the port and exits non-zero instead.
- **Lifetime energy starts at 12345.6 kWh**, not 0, so registers 3–4 have a
  non-zero high word from the first poll. Starting at zero leaves the uint32 pair
  all-zero for minutes and would mask a byte-order bug in the 32-bit path.
- **`dialout` membership** is needed to open `/dev/ttyUSB0` without sudo. Already
  set on `rpi-byte`.
- **Adapter not enumerating?** `lsusb` should list a `1a86:7523` CH340. If
  `/dev/ttyUSB0` is missing, check `dmesg | tail`.
