# Modbus TCP Device — Design Document

**Component:** `components/modbus_device` (extends Day 5)
**Author:** Luca Agrippino
**Date:** 2026-07-26
**Status:** Design

---

## 1. Purpose

This module adds the TCP transport to the Modbus device component, implementing
the same `IModbusDevice` interface as `ModbusRtuDevice`. Instead of UART/RS-485,
it communicates over Wi-Fi using TCP sockets. The slave is a pymodbus TCP server
running on the Raspberry Pi.

**Requirements covered:** REQ-F-007

---

## 2. Modbus TCP vs Modbus RTU

| Aspect | RTU | TCP |
|--------|-----|-----|
| Physical layer | UART / RS-485 (serial) | Ethernet / Wi-Fi (TCP/IP) |
| Framing | `[addr][func][data][CRC]` | `[MBAP header][func][data]` |
| Error checking | CRC-16 appended to frame | TCP provides reliable delivery |
| Slave addressing | Slave address in frame (1–247) | Unit ID in MBAP header + IP/port |
| Connection | Always on (bus) | Connect/disconnect per session |
| Speed | 9600–115200 baud | Wi-Fi throughput (~Mbps) |
| Direction control | DE/RE pin (half-duplex) | Full-duplex TCP |
| Max distance | 1200 m (RS-485) | Network range (Wi-Fi/Ethernet) |

### MBAP Header (Modbus Application Protocol)

TCP replaces the slave address + CRC with a 7-byte header:

```
[Transaction ID:2][Protocol ID:2][Length:2][Unit ID:1][Function:1][Data:N]
      0x0001           0x0000      N+1        0x01       0x03      ...
```

| Field | Size | Value | Purpose |
|-------|------|-------|---------|
| Transaction ID | 2 bytes | Incrementing counter | Match requests to responses |
| Protocol ID | 2 bytes | 0x0000 | Always 0 for Modbus |
| Length | 2 bytes | Remaining bytes | Unit ID + Function + Data |
| Unit ID | 1 byte | Slave address | Same as RTU slave addr (1–247) |

No CRC needed — TCP guarantees reliable, ordered delivery.

---

## 3. Interface (Same as RTU)

```cpp
class ModbusTcpDevice : public IModbusDevice {
public:
    ModbusTcpDevice(std::string ip_addr, uint16_t port, uint8_t unit_id,
                    std::vector<RegisterDef> register_map);
    ~ModbusTcpDevice() override;

    ModbusTcpDevice(const ModbusTcpDevice&) = delete;
    ModbusTcpDevice& operator=(const ModbusTcpDevice&) = delete;
    ModbusTcpDevice(ModbusTcpDevice&&) = delete;
    ModbusTcpDevice& operator=(ModbusTcpDevice&&) = delete;

    esp_err_t init() override;
    std::vector<Reading> readRegisters() override;
    std::string_view name() const override { return "modbus_tcp"; }

private:
    std::string ip_addr_;
    uint16_t port_;
    uint8_t unit_id_;
    std::vector<RegisterDef> register_map_;
    int sock_{-1};
    uint16_t transaction_id_{0};

    esp_err_t connectSocket();
    void closeSocket();
    esp_err_t sendRequest(uint8_t func, uint16_t start_reg, uint16_t count,
                          uint8_t* response, size_t* resp_len);
};
```

### Key Differences from ModbusRtuDevice

| Member | RTU | TCP |
|--------|-----|-----|
| Transport config | `uart_port_t`, GPIO pins | `std::string ip_addr_`, `uint16_t port_` |
| Connection handle | UART port number | `int sock_` (BSD socket FD) |
| Error checking | CRC-16 computation | None (TCP handles it) |
| Direction control | DE/RE pin via UART RS-485 mode | Not needed (full-duplex) |
| Transaction tracking | Not needed (one request at a time on bus) | `transaction_id_` for request/response matching |

---

## 4. RAII Lifecycle

| Phase | Action | API |
|-------|--------|-----|
| Constructor | Store IP, port, unit ID, register map | — |
| `init()` | Create TCP socket, connect to slave | `socket()`, `connect()` |
| `readRegisters()` | Build MBAP frame, send, receive, parse | `send()`, `recv()` |
| Destructor | Close TCP socket | `close()` |

### Socket Connection

```cpp
esp_err_t ModbusTcpDevice::connectSocket() {
    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port_);
    inet_pton(AF_INET, ip_addr_.c_str(), &dest.sin_addr);

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ < 0) {
        ESP_LOGE(TAG, "Socket creation failed: errno %d", errno);
        return ESP_FAIL;
    }

    // Set receive timeout
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int err = connect(sock_, (struct sockaddr*)&dest, sizeof(dest));
    if (err != 0) {
        ESP_LOGE(TAG, "Connect to %s:%d failed: errno %d",
                 ip_addr_.c_str(), port_, errno);
        close(sock_);
        sock_ = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connected to Modbus TCP slave at %s:%d",
             ip_addr_.c_str(), port_);
    return ESP_OK;
}

ModbusTcpDevice::~ModbusTcpDevice() {
    closeSocket();
}

void ModbusTcpDevice::closeSocket() {
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}
```

---

## 5. TCP Request/Response

### Building the MBAP Frame

```cpp
esp_err_t ModbusTcpDevice::sendRequest(uint8_t func, uint16_t start_reg,
                                        uint16_t count,
                                        uint8_t* response, size_t* resp_len) {
    uint8_t request[12];
    uint16_t tid = transaction_id_++;

    // MBAP header
    request[0] = tid >> 8;            // Transaction ID high
    request[1] = tid & 0xFF;          // Transaction ID low
    request[2] = 0x00;                // Protocol ID high
    request[3] = 0x00;                // Protocol ID low
    request[4] = 0x00;                // Length high
    request[5] = 0x06;                // Length low (6 bytes follow)
    request[6] = unit_id_;            // Unit ID

    // PDU (same as RTU, without CRC)
    request[7] = func;                // Function code
    request[8] = start_reg >> 8;      // Start register high
    request[9] = start_reg & 0xFF;    // Start register low
    request[10] = count >> 8;         // Register count high
    request[11] = count & 0xFF;       // Register count low

    int sent = send(sock_, request, sizeof(request), 0);
    if (sent != sizeof(request)) {
        ESP_LOGE(TAG, "Send failed: errno %d", errno);
        return ESP_FAIL;
    }

    // Receive response: MBAP header (7 bytes) + PDU
    int received = recv(sock_, response, 256, 0);
    if (received <= 0) {
        ESP_LOGE(TAG, "Recv failed: errno %d", errno);
        return ESP_ERR_TIMEOUT;
    }

    // Validate transaction ID matches
    uint16_t resp_tid = (response[0] << 8) | response[1];
    if (resp_tid != tid) {
        ESP_LOGW(TAG, "Transaction ID mismatch: sent %d, got %d", tid, resp_tid);
    }

    *resp_len = received;
    return ESP_OK;
}
```

### Register Parsing (Shared with RTU)

The register data in the response starts at a different offset than RTU:
- **RTU:** data starts at byte 3 (after addr + func + byte_count)
- **TCP:** data starts at byte 9 (after MBAP header 7 + func + byte_count)

The scaling logic (`scaleValue`, `scaleValue32`) is identical.

---

## 6. Wi-Fi Dependency

Modbus TCP requires an active Wi-Fi connection. The task waits for connectivity
before starting:

```cpp
void modbusTcpTask(void* param) {
    auto* ctx = static_cast<ModbusTcpTaskContext*>(param);

    // Block until Wi-Fi is connected
    xEventGroupWaitBits(ctx->wifi_event_group,
                        WifiManager::CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_ERROR_CHECK(ctx->device->init());

    const TickType_t period = pdMS_TO_TICKS(2000);
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        // Check if still connected
        EventBits_t bits = xEventGroupGetBits(ctx->wifi_event_group);
        if (!(bits & WifiManager::CONNECTED_BIT)) {
            ESP_LOGW(TAG, "Wi-Fi disconnected, skipping Modbus TCP poll");
            vTaskDelayUntil(&last_wake, period);
            continue;
        }

        std::vector<Reading> readings = ctx->device->readRegisters();
        xQueueOverwrite(ctx->mailbox, &readings);

        vTaskDelayUntil(&last_wake, period);
    }
}
```

### Reconnection Strategy

If the TCP socket disconnects (slave goes down, network issue):

```cpp
std::vector<Reading> ModbusTcpDevice::readRegisters() {
    // Try to reconnect if socket is closed
    if (sock_ < 0) {
        if (connectSocket() != ESP_OK) {
            // Return all ERROR readings
            return makeErrorReadings();
        }
    }

    // ... send request ...
    esp_err_t err = sendRequest(0x03, 0, 6, response, &resp_len);
    if (err != ESP_OK) {
        closeSocket();  // Force reconnect on next cycle
        return makeErrorReadings();
    }

    // ... parse and return readings ...
}
```

### Task Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Priority | 4 | Same as Modbus RTU |
| Stack | 4096 bytes | Socket operations use stack |
| Period | 2000 ms | 0.5 Hz poll rate |
| Core | 1 | Keep off core 0 (Wi-Fi) |

---

## 7. pymodbus TCP Slave (Raspberry Pi)

```python
#!/usr/bin/env python3
# tools/pymodbus_slave/tcp_slave.py

from pymodbus.server import StartTcpServer
from pymodbus.datastore import (
    ModbusSequentialDataBlock,
    ModbusSlaveContext,
    ModbusServerContext,
)
import threading, time, random

store = ModbusSlaveContext(
    hr=ModbusSequentialDataBlock(0, [0]*6),
)
context = ModbusServerContext(slaves={1: store}, single=False)

def update_registers():
    """Simulate changing inverter values."""
    energy = 0
    while True:
        voltage = int(485 + random.randint(-10, 10))
        current = int(1050 + random.randint(-50, 50))
        power = voltage * current // 1000
        energy += power
        status = 1

        store.setValues(3, 0, [voltage, current, power,
                                (energy >> 16) & 0xFFFF,
                                energy & 0xFFFF,
                                status])
        time.sleep(1)

threading.Thread(target=update_registers, daemon=True).start()

StartTcpServer(
    context=context,
    address=("0.0.0.0", 502),  # Listen on all interfaces, port 502
)
```

### Running on the Raspberry Pi

```bash
# Port 502 requires root (privileged port)
sudo python3 tcp_slave.py

# Or use port 1502 (non-privileged) and update Kconfig
python3 tcp_slave.py  # after changing address to ("0.0.0.0", 1502)
```

---

## 8. Kconfig

```kconfig
menu "Modbus TCP Configuration"

    config MODBUS_TCP_IP
        string "Slave IP address"
        default "192.168.4.100"

    config MODBUS_TCP_PORT
        int "Slave TCP port"
        default 502

    config MODBUS_TCP_UNIT_ID
        int "Unit ID (slave address)"
        default 1
        range 1 247

    config MODBUS_TCP_POLL_MS
        int "Poll period (ms)"
        default 2000

endmenu
```

---

## 9. Error Handling

| Error | Detection | Response |
|-------|-----------|----------|
| Socket creation fails | `socket()` returns -1 | Log error, return ERROR readings |
| Connection refused | `connect()` fails | Log error, retry next cycle |
| Send fails | `send()` returns -1 or partial | Close socket, reconnect next cycle |
| Recv timeout | `recv()` returns 0 or -1 | Close socket, return ERROR readings |
| Transaction ID mismatch | Response TID ≠ request TID | Log warning, accept data (non-fatal) |
| Wi-Fi disconnected | Event group check | Skip poll, wait for reconnection |

---

## 10. Updated CMakeLists.txt

```cmake
idf_component_register(
    SRCS "src/ModbusRtuDevice.cpp"
         "src/ModbusTcpDevice.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_netif
    PRIV_REQUIRES common
)
```

`esp_netif` added for socket functions (`lwip/sockets.h`).

---

## 11. Integration Tests

| Test | Description | Type |
|------|-------------|------|
| TCP connect | Start pymodbus TCP slave, connect, verify socket established | On-target + RPi |
| TCP read | Read registers, verify scaled values match pymodbus | On-target + RPi |
| Reconnect | Kill pymodbus, verify ERROR readings, restart pymodbus, verify recovery | On-target + RPi |
| Wi-Fi dependency | Start task without Wi-Fi, verify it blocks on event group | On-target |
| Same interface | Verify `readRegisters()` output format matches RTU format | On-target |

---

## 12. Stack and Heap Budget

| Metric | Target | Measurement Method |
|--------|--------|--------------------|
| Stack HWM | ≥ 25% of 4096 = 1024 bytes free | `uxTaskGetStackHighWaterMark()` |
| Heap impact | Socket internals ~1 KB + vector ~160 bytes per cycle | `esp_get_free_heap_size()` |
