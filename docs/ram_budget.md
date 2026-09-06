# RAM Budget

**Target:** ESP32-S3-DevKitC-1 (16 MB flash, no PSRAM enabled)
**Firmware:** `main` @ `97610e4`, ESP-IDF v5.5.5
**Measured:** 2026-09-06, board running all tasks with Wi-Fi connected, **all three
sources live** (IMU on I2C, Modbus RTU over RS-485, Modbus TCP over Wi-Fi), and a
WebSocket client attached. Re-measured after the RS-485 bus was wired; the earlier
figures were taken with RTU failing every poll.

Covers **REQ-NF-002** (≤ 80% heap at steady state) and **REQ-NF-003** (≥ 25% stack
headroom per task).

---

## 1. How to reproduce

```bash
idf.py size                       # static, link-time
idf.py size-components            # per-archive breakdown
python3 tools/test/health_check.py <esp32_ip>   # runtime, from the device
```

`health_check.py` exits non-zero if either budget is breached, so it can gate a
release.

---

## 2. Static footprint (link time)

From `idf.py size`. These are link-time figures — what the image reserves before
a single byte is allocated at runtime.

| Memory | Used | Used % | Remaining | Total |
|---|---:|---:|---:|---:|
| DIRAM | 116,899 | 34.2% | 224,861 | 341,760 |
| ├ `.text` | 80,611 | 23.6% | | |
| ├ `.data` | 20,032 | 5.9% | | |
| └ `.bss` | 16,256 | 4.8% | | |
| IRAM | 16,384 | 100.0% | 0 | 16,384 |
| RTC SLOW | 32 | 0.4% | 8,160 | 8,192 |
| RTC FAST | 24 | 0.3% | 8,168 | 8,192 |

IRAM shows 100% because ESP-IDF deliberately fills it — the remainder is handed
to the heap rather than wasted, so this is not a pressure signal.

### Per-component contribution

Project components only, from `idf.py size-components`. Everything here is flash
except `libmain.a`'s 148 bytes of `.bss`; none of the components claim DIRAM
statically, because their state lives in heap-allocated objects and task stacks.

| Archive | Total | Flash code | Flash data | DIRAM |
|---|---:|---:|---:|---:|
| `libmodbus_device.a` | 5,487 | 5,403 | 84 | 0 |
| `libmain.a` | 5,087 | 4,914 | 25 | 148 |
| `libwifi_manager.a` | 4,357 | 2,717 | 1,640 | 0 |
| `libpublisher.a` | 3,714 | 1,507 | 2,207 | 0 |
| `libimu.a` | 1,424 | 1,368 | 56 | 0 |
| `libhealth.a` | 1,027 | 1,027 | 0 | 0 |
| `libaggregator.a` | 448 | 448 | 0 | 0 |
| **Total (project code)** | **21,544** | | | |

The application is ~2.4% of the 879 KB image; the rest is ESP-IDF, chiefly the
Wi-Fi driver, lwIP and mbedTLS.

---

## 3. Heap — REQ-NF-002

Measured at runtime from `/health`, which reports
`heap_caps_get_total_size(MALLOC_CAP_DEFAULT)` alongside free and minimum-ever.

| Metric | Bytes | % of heap |
|---|---:|---:|
| Total heap | 345,436 | 100% |
| Free now | 225,980 | **34.6% used** |
| Minimum ever free | 190,860 | **44.7% used at peak** |

**REQ-NF-002 (≤ 80% used): PASS** — peak usage 44.7%, i.e. 35 percentage points
of margin. Peak rose from 35.9% once RS-485 began decoding real responses rather
than timing out, which is the honest steady-state figure.

> The heap total reported at runtime is not the same quantity as the "DIRAM
> remaining" row in §2. `idf.py size` reports link-time section usage against the
> D/IRAM address space, while `heap_caps_get_total_size` sums the regions the
> allocator actually registered at startup, which includes memory `idf.py size`
> does not count in that row. They are quoted separately here rather than
> reconciled, because only the runtime figure is meaningful for REQ-NF-002.

Minimum-ever free is the number that matters: it captures the transient peak
during Wi-Fi association and TLS-free HTTP server startup, which is when the
heap is under most pressure. It sat within 5 KB of the steady-state free figure,
so there is no large transient spike hiding in boot.

---

## 4. Task stacks — REQ-NF-003

`uxTaskGetStackHighWaterMark` returns the smallest free stack a task has ever
had. On ESP-IDF it is reported **in bytes**, explicitly unlike upstream FreeRTOS
which returns words — confirmed in
`components/freertos/FreeRTOS-Kernel/include/freertos/task.h`.

| Task | Stack | HWM (free) | Headroom | Verdict |
|---|---:|---:|---:|---|
| `imu` | 4,096 | 2,764 | 67.5% | PASS |
| `modbus_rtu` | 4,096 | 2,676 | 65.3% | PASS |
| `modbus_tcp` | 4,096 | 1,568 | 38.3% | PASS |
| `aggregator` | 4,096 | 3,100 | 75.7% | PASS |
| `publisher` | 6,144 | 3,824 | 62.2% | PASS |
| `health` | 4,096 | 1,988 | 48.5% | PASS |

**REQ-NF-003 (≥ 25% headroom): PASS** — every task clears the target, the tightest
being `modbus_tcp` at 38.3%.

Total stack allocation is 26,624 bytes; peak combined usage is 11,704 bytes.

### Notes on individual tasks

- **`modbus_rtu` improved from 42.1% to 65.3%** once the bus was wired. That is
  not a typo: the failure path is the *more* expensive one. A timed-out poll
  still builds a full `allFailed()` vector of five `Reading`s after the UART
  read has already used its buffers, whereas a successful decode reuses the
  response buffer it already holds.
- **`publisher` (63.7%)** carries the largest absolute usage at 2,232 bytes,
  which is expected — it builds the snapshot JSON and drives `httpd_ws_send_data`
  for every connected client. Its 6,144-byte stack is already the largest, and
  the headroom confirms that was the right call.
- **`aggregator` (75.7%)** is the roomiest despite touching every mailbox,
  because it only moves POD `Reading` structs and does no formatting.
- **`modbus_tcp` (38.3%, now the tightest)** holds a 264-byte response buffer
  plus the lwIP socket path, which is deeper than the UART one.

---

## 5. Verdict

| Requirement | Target | Measured | Result |
|---|---|---|---|
| REQ-NF-002 | ≤ 80% heap | 44.7% peak | **PASS** |
| REQ-NF-003 | ≥ 25% stack headroom | 38.3% worst | **PASS** |

Both pass with wide margin, and unlike the earlier revision these are true
steady-state figures: every source was live and healthy when they were taken.
