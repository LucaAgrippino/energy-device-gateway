# Energy Device Gateway

ESP32-S3 firmware that collects data from a local sensor (IMU over I2C) and
industrial devices (Modbus RTU over RS-485, Modbus TCP over Wi-Fi), aggregates
the readings into periodic snapshots, and streams them in real time over
WebSocket to a browser dashboard.

Built as a V-model exercise: every component has a `DESIGN.md` written before its
implementation, and tests at unit, integration, system and acceptance level.

---

## Status

| Component | Tag | Hardware-validated |
|---|---|---|
| `common` | `common-v1.0` | n/a (shared types) |
| `imu` | `imu-v1.0` | Yes — MPU9150 on I2C0 |
| `wifi_manager` | `wifi_manager-v1.1` | Yes — STA, AP fallback, NVS provisioning |
| `aggregator` | `aggregator-v1.1` | Yes |
| `publisher` | `publisher-v1.1` | Yes — dashboard + `/ws` + latency measured |
| `modbus_device` (TCP) | `modbus_device-v1.1` | Yes — against a pymodbus slave |
| `modbus_device` (RTU) | `modbus_device-v1.0` | Yes — over real RS-485 to a pymodbus slave |
| `health` | `health-v1.0` | Yes — `/health` |

On-target unit tests: **20 passing, 0 failing**.
System tests: **7 passing, 1 skipped** (Wi-Fi reconnect needs router access).
See [docs/day7_system_test_plan.md](docs/day7_system_test_plan.md).

---

## Quick start

### Prerequisites

- ESP-IDF **v5.5** (this project is pinned to v5.5.x; a v6.x `IDF_PATH` will not
  build it)
- ESP32-S3-DevKitC-1
- Drotek MPU9150 on I2C (SDA `GPIO1`, SCL `GPIO2`, address `0x69`)
- Optional: TTL-to-RS-485 module + USB-RS-485 adapter, and a Raspberry Pi
  running the pymodbus slaves in [`tools/pymodbus_slave/`](tools/pymodbus_slave/)

### Build and flash

```bash
. $HOME/esp/esp-idf-v5.5/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### Configure

```bash
idf.py menuconfig
```

- **Wi-Fi Manager Configuration** — AP-mode SSID/password, STA timeout, retry
  count, provisioning port, modem power save
- **Modbus RTU Configuration** — UART port, TX/RX/DE-RE pins, baud, slave address
- **Modbus TCP Configuration** — slave IP, port, unit id, poll period
- **Health Monitor Configuration** — log period, serial logging on/off

Station credentials are **not** build-time options: they live in NVS and are set
by provisioning (below).

### Provisioning Wi-Fi

With no stored credentials the gateway starts an AP after the STA timeout:

1. Join **`EDG-Setup`** (default password `edg12345`, see `CONFIG_WIFI_AP_PASSWORD`)
2. Open `http://192.168.4.1:8080/` and submit your SSID and password

They are saved to NVS and survive reflashing — only `idf.py erase-flash` clears
them. Or POST directly:

```bash
curl --data-urlencode "ssid=YourSSID" --data-urlencode "password=YourPassword" \
     http://192.168.4.1:8080/save
```

---

## Using it

Once connected, the board serves three endpoints on port 80:

| Path | Purpose |
|---|---|
| `/` | HTML dashboard |
| `/ws` | WebSocket telemetry, one JSON snapshot every 500 ms |
| `/health` | Diagnostics: uptime, heap, Wi-Fi RSSI, per-task stack high-water marks |

```console
$ curl -s http://<ip>/health | python3 -m json.tool
{
    "uptime_s": 43,
    "heap": { "free": 226232, "min_ever": 221300, "total": 345436 },
    "wifi": { "rssi": -50, "connected": true },
    "tasks": [ { "name": "imu", "stack_hwm": 2804, "stack_total": 4096 }, ... ]
}
```

A telemetry frame carries every source in one snapshot, each reading tagged
`st`: `0` OK, `1` TIMEOUT, `2` ERROR.

```json
{"ts":36230023,"readings":[
  {"src":"imu.accel_x","val":-1.410,"st":0},
  {"src":"modbus_tcp.voltage","val":432.400,"st":0},
  {"src":"modbus_rtu.voltage","val":0.000,"st":2}
]}
```

---

## Testing

```bash
# On-target unit tests (imu, modbus_device, health)
cd $IDF_PATH/tools/unit-test-app
idf.py -T imu -T modbus_device -T health \
       -D EXTRA_COMPONENT_DIRS=<repo>/components \
       -D SDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults;<repo>/sdkconfig.defaults" \
       build flash monitor
# then enter '*' at the prompt to run all

# Host tests (aggregator logic, no hardware) — needs libbsd-dev
cd components/aggregator/host_test && idf.py --preview set-target linux && idf.py build monitor

# System and acceptance tests against a running board
cd tools/test
python3 system_test.py  <esp32_ip>   # ST-001..ST-008
python3 health_check.py <esp32_ip>   # REQ-NF-002, REQ-NF-003
python3 latency_test.py <esp32_ip>   # REQ-NF-004
python3 ws_client.py    <esp32_ip>   # live frame dump
```

The Modbus slave simulators run on the Raspberry Pi:

```bash
python3 tools/pymodbus_slave/tcp_slave.py --host 0.0.0.0 --port 5020
python3 tools/pymodbus_slave/rtu_slave.py            # needs /dev/ttyUSB*
```

They require **pymodbus 3.8.x** — 3.9 removed the runtime register-mutation API
these simulators depend on. See `tools/pymodbus_slave/requirements.txt`.

---

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) — system design, task model, data flow
- [VISION.md](VISION.md) — requirements, V-model process, development plan
- [STATE.md](STATE.md) — current state, decisions, blockers, session history
- [docs/ram_budget.md](docs/ram_budget.md) — measured heap and stack budget
- [CLAUDE.md](CLAUDE.md) — coding standards and project rules
- `components/*/DESIGN.md` — per-component design, written before implementation

---

## Known limitations

- **Wi-Fi reconnect after losing an established link** is untested; AP fallback
  and first-connect retry are both verified.
- 2.4 GHz only, as the ESP32-S3 radio has no 5 GHz support.

## License

MIT
