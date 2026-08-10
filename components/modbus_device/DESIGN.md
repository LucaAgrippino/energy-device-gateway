# Modbus Device Component — Design Document

**Component:** `components/modbus_device`
**Author:** Luca Agrippino
**Date:** 2026-07-25
**Status:** Design

---

## 1. Purpose

This component provides an abstraction layer for reading holding registers from
Modbus slave devices. It supports two transports behind a common interface:
Modbus RTU (UART/RS-485, Day 5) and Modbus TCP (Wi-Fi, Day 6). The initial
implementation targets a simulated solar inverter running on the Raspberry Pi
via pymodbus.

**Requirements covered:** REQ-F-006 (RTU), REQ-F-007 (TCP)

---

## 2. Modbus Protocol Primer

Modbus is a request-response protocol used in industrial automation. Key concepts:

| Concept | Description |
|---------|-------------|
| Master | Initiates communication (our ESP32) |
| Slave | Responds to requests (our Raspberry Pi running pymodbus) |
| Slave address | 1–247, identifies the slave on the bus |
| Function code | What to do: 0x03 = Read Holding Registers |
| Register | 16-bit (2 bytes) data unit, addressed 0–65535 |
| RTU frame | `[addr][func][data][CRC16]` — binary, over UART/RS-485 |
| TCP frame | `[MBAP header][func][data]` — binary, over TCP/IP |

### RS-485 Physical Layer (RTU Only)

RS-485 is a differential signalling standard for multi-drop serial communication.
Key properties:
- Two wires: A and B (differential pair)
- Half-duplex: only one device transmits at a time
- Up to 32 devices on one bus
- Up to 1200 m cable length
- Requires a direction control pin (DE/RE) to switch between transmit and receive

```
ESP32-S3                   RS-485 Module              RS-485 Bus
GPIO 17 (TX) ─────→ DI                    A ─────── A (to RPi adapter)
GPIO 18 (RX) ←───── RO                    B ─────── B (to RPi adapter)
GPIO  8 (DE/RE) ──→ DE + /RE (tied)
                                    GND ─── GND
                                    VCC ─── 3.3V
```

**DE/RE control:** The ESP32 drives GPIO 8 HIGH to transmit, LOW to receive.
ESP-IDF's UART driver handles this automatically when configured in
`UART_MODE_RS485_HALF_DUPLEX` — the RTS pin toggles DE/RE.

---

## 3. Interface

```cpp
// IModbusDevice.hpp
#pragma once

#include <vector>
#include <string_view>
#include "esp_err.h"
#include "Reading.hpp"

class IModbusDevice {
public:
    virtual ~IModbusDevice() = default;
    virtual esp_err_t init() = 0;
    virtual std::vector<Reading> readRegisters() = 0;
    virtual std::string_view name() const = 0;
};
```

### Why Return std::vector<Reading>?

Unlike the IMU (which returns a single `ImuReading`), a Modbus device returns
multiple register values per read cycle. The vector lets each device return a
variable number of readings based on its register map.

---

## 4. ModbusRtuDevice

```cpp
// ModbusRtuDevice.hpp
#pragma once

#include "IModbusDevice.hpp"
#include "driver/uart.h"
#include "driver/gpio.h"

struct RegisterDef {
    uint16_t        address;
    std::string_view name;       // e.g. "voltage", "current"
    float           scale;       // Raw × scale = physical value
    uint8_t         reg_count;   // 1 for uint16, 2 for uint32
};

class ModbusRtuDevice : public IModbusDevice {
public:
    ModbusRtuDevice(uart_port_t port, uint8_t slave_addr,
                    gpio_num_t tx_pin, gpio_num_t rx_pin,
                    gpio_num_t de_re_pin,
                    std::vector<RegisterDef> register_map);
    ~ModbusRtuDevice() override;

    ModbusRtuDevice(const ModbusRtuDevice&) = delete;
    ModbusRtuDevice& operator=(const ModbusRtuDevice&) = delete;
    ModbusRtuDevice(ModbusRtuDevice&&) = delete;
    ModbusRtuDevice& operator=(ModbusRtuDevice&&) = delete;

    esp_err_t init() override;
    std::vector<Reading> readRegisters() override;
    std::string_view name() const override { return "modbus_rtu"; }

private:
    uart_port_t port_;
    uint8_t slave_addr_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    gpio_num_t de_re_pin_;
    std::vector<RegisterDef> register_map_;

    esp_err_t sendRequest(uint8_t func, uint16_t start_reg, uint16_t count,
                          uint8_t* response, size_t* resp_len);
    uint16_t crc16(const uint8_t* data, size_t len);
    float scaleValue(uint16_t raw, float scale);
    float scaleValue32(uint16_t high, uint16_t low, float scale);
};
```

---

## 5. RAII Lifecycle

| Phase | Action | ESP-IDF API |
|-------|--------|-------------|
| Constructor | Store config, no hardware init | — |
| `init()` | Configure UART, set RS-485 mode, configure DE/RE pin | `uart_driver_install()`, `uart_param_config()`, `uart_set_pin()`, `uart_set_mode()` |
| `readRegisters()` | Build Modbus frame, send, receive, parse, scale | `uart_write_bytes()`, `uart_read_bytes()` |
| Destructor | Delete UART driver | `uart_driver_delete()` |

### UART Configuration

```cpp
esp_err_t ModbusRtuDevice::init() {
    uart_config_t uart_cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(port_, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(port_, tx_pin_, rx_pin_,
                                  de_re_pin_, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(port_, 256, 256, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_set_mode(port_, UART_MODE_RS485_HALF_DUPLEX));

    return ESP_OK;
}
```

**`UART_MODE_RS485_HALF_DUPLEX`** — tells the UART driver to automatically toggle
the RTS pin (which we wired to DE/RE) for direction control. HIGH during transmit,
LOW during receive. No manual GPIO toggling needed.

---

## 6. Register Map Configuration

The register map is passed at construction, making the device configurable for
different slave types:

```cpp
// In app_main — configure for our simulated inverter:
std::vector<RegisterDef> inverter_regs = {
    {0, "voltage",      0.1f,  1},   // Reg 0: uint16, ×0.1 → Volts
    {1, "current",      0.01f, 1},   // Reg 1: uint16, ×0.01 → Amps
    {2, "power",        1.0f,  1},   // Reg 2: uint16, ×1 → Watts
    {3, "energy_total", 0.1f,  2},   // Reg 3-4: uint32, ×0.1 → kWh
    {5, "status",       1.0f,  1},   // Reg 5: uint16, raw enum
};

auto rtu = std::make_unique<ModbusRtuDevice>(
    UART_NUM_1, 1,               // UART port 1, slave address 1
    GPIO_NUM_17, GPIO_NUM_18,    // TX, RX
    GPIO_NUM_8,                  // DE/RE
    std::move(inverter_regs)
);
```

---

## 7. Modbus RTU Frame Format

### Request (Master → Slave)

```
[SlaveAddr:1][FuncCode:1][StartReg:2][RegCount:2][CRC:2]
     0x01         0x03       0x00 0x00    0x00 0x06   CRC16

= "Slave 1, read 6 holding registers starting at register 0"
```

### Response (Slave → Master)

```
[SlaveAddr:1][FuncCode:1][ByteCount:1][Data:N][CRC:2]
     0x01         0x03        0x0C      12 bytes   CRC16

= "Slave 1, here are 12 bytes (6 registers × 2 bytes each)"
```

### CRC-16/Modbus

Every RTU frame ends with a 2-byte CRC (Cyclic Redundancy Check). The algorithm
is CRC-16/Modbus (polynomial 0xA001, init 0xFFFF):

```cpp
uint16_t ModbusRtuDevice::crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}
```

---

## 8. Reading and Scaling

```cpp
std::vector<Reading> ModbusRtuDevice::readRegisters() {
    std::vector<Reading> readings;
    readings.reserve(register_map_.size());
    int64_t now = esp_timer_get_time();

    // Read all registers in one Modbus request (registers 0–5)
    uint8_t response[32];
    size_t resp_len = 0;
    esp_err_t err = sendRequest(0x03, 0, 6, response, &resp_len);

    if (err != ESP_OK) {
        // All readings from this device are ERROR
        for (const auto& reg : register_map_) {
            readings.push_back(Reading{
                reg.name, 0.0f, now, Reading::Status::ERROR
            });
        }
        return readings;
    }

    // Parse response data (skip addr + func + byte_count = 3 bytes)
    const uint8_t* data = response + 3;

    for (const auto& reg : register_map_) {
        float value;
        size_t offset = reg.address * 2;  // Each register is 2 bytes

        if (reg.reg_count == 1) {
            uint16_t raw = (data[offset] << 8) | data[offset + 1];
            value = scaleValue(raw, reg.scale);
        } else {
            // uint32 from two consecutive registers (big-endian)
            uint16_t high = (data[offset] << 8) | data[offset + 1];
            uint16_t low = (data[offset + 2] << 8) | data[offset + 3];
            value = scaleValue32(high, low, reg.scale);
        }

        readings.push_back(Reading{
            reg.name, value, now, Reading::Status::OK
        });
    }

    return readings;
}

float ModbusRtuDevice::scaleValue(uint16_t raw, float scale) {
    return static_cast<float>(raw) * scale;
}

float ModbusRtuDevice::scaleValue32(uint16_t high, uint16_t low, float scale) {
    uint32_t raw = (static_cast<uint32_t>(high) << 16) | low;
    return static_cast<float>(raw) * scale;
}
```

---

## 9. pymodbus RTU Slave (Raspberry Pi)

```python
#!/usr/bin/env python3
# tools/pymodbus_slave/rtu_slave.py

from pymodbus.server import StartSerialServer
from pymodbus.datastore import (
    ModbusSequentialDataBlock,
    ModbusSlaveContext,
    ModbusServerContext,
)
import threading, time, random

# Register layout matches VISION.md §7.2
# Regs: 0=voltage, 1=current, 2=power, 3-4=energy_total, 5=status
store = ModbusSlaveContext(
    hr=ModbusSequentialDataBlock(0, [0]*6),  # 6 holding registers
)
context = ModbusServerContext(slaves={1: store}, single=False)

def update_registers():
    """Simulate changing inverter values."""
    energy = 0
    while True:
        voltage = int(485 + random.randint(-10, 10))     # ~48.5V (×0.1)
        current = int(1050 + random.randint(-50, 50))     # ~10.50A (×0.01)
        power = voltage * current // 1000                  # ~W
        energy += power
        status = 1  # running

        store.setValues(3, 0, [voltage, current, power,
                                (energy >> 16) & 0xFFFF,
                                energy & 0xFFFF,
                                status])
        time.sleep(1)

threading.Thread(target=update_registers, daemon=True).start()

StartSerialServer(
    context=context,
    port="/dev/ttyUSB0",
    baudrate=9600,
    stopbits=1,
    bytesize=8,
    parity="N",
)
```

### Running on the Raspberry Pi

```bash
pip3 install pymodbus
python3 rtu_slave.py
```

---

## 10. ModbusRtuTask

```cpp
void modbusRtuTask(void* param) {
    auto* ctx = static_cast<ModbusRtuTaskContext*>(param);
    const TickType_t period = pdMS_TO_TICKS(1000);
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        std::vector<Reading> readings = ctx->device->readRegisters();

        // Write all readings to the aggregator mailbox
        // Option: single mailbox with the entire vector
        // or multiple mailboxes per register
        xQueueOverwrite(ctx->mailbox, &readings);

        vTaskDelayUntil(&last_wake, period);
    }
}
```

### Task Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Priority | 4 | Below IMU, above aggregator |
| Stack | 4096 bytes | UART I/O + vector operations |
| Period | 1000 ms | 1 Hz poll rate |
| Core | 1 | Keep off core 0 (Wi-Fi) |

---

## 11. Kconfig

```kconfig
menu "Modbus RTU Configuration"

    config MODBUS_RTU_UART_PORT
        int "UART port number"
        default 1

    config MODBUS_RTU_BAUD_RATE
        int "Baud rate"
        default 9600

    config MODBUS_RTU_SLAVE_ADDR
        int "Slave address"
        default 1
        range 1 247

    config MODBUS_RTU_TX_PIN
        int "TX GPIO pin"
        default 17

    config MODBUS_RTU_RX_PIN
        int "RX GPIO pin"
        default 18

    config MODBUS_RTU_DE_RE_PIN
        int "DE/RE GPIO pin"
        default 8

endmenu
```

---

## 12. Error Handling

| Error | Detection | Response |
|-------|-----------|----------|
| UART init fails | `uart_driver_install` returns error | Fatal — `ESP_ERROR_CHECK` |
| No response from slave | `uart_read_bytes` returns 0 (timeout) | All readings → Status::ERROR |
| CRC mismatch | Computed CRC ≠ received CRC | Log warning, all readings → Status::ERROR |
| Partial response | `resp_len < expected` | Log warning, all readings → Status::ERROR |
| Slave exception | Function code has error bit set (0x83) | Log error code, all readings → Status::ERROR |
| RS-485 bus contention | Garbled data | CRC check catches it; verify with USB sniffer |

---

## 13. File Structure

```
components/modbus_device/
├── CMakeLists.txt
├── DESIGN.md              ← this file
├── Kconfig
├── include/
│   ├── IModbusDevice.hpp
│   ├── ModbusRtuDevice.hpp
│   └── ModbusTcpDevice.hpp     ← Day 6
├── src/
│   ├── ModbusRtuDevice.cpp
│   └── ModbusTcpDevice.cpp     ← Day 6
└── test/
    └── test_modbus.cpp

tools/pymodbus_slave/
├── rtu_slave.py
└── tcp_slave.py                ← Day 6
```

### CMakeLists.txt

```cmake
idf_component_register(
    SRCS "src/ModbusRtuDevice.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES common
)
```

---

## 14. Integration Tests

| Test | Description | Type |
|------|-------------|------|
| RTU read | Start pymodbus slave, read registers, verify scaled values | On-target + RPi |
| CRC validation | Send valid request, verify CRC in response matches | On-target + RPi |
| Slave timeout | Don't start pymodbus, verify all readings → ERROR | On-target |
| Register map | Verify voltage, current, power, energy, status values match pymodbus | On-target + RPi |
| Bus sniff | Use USB-to-RS-485 adapter on PC to capture and verify frames | On-target + RPi + PC |

---

## 15. Stack and Heap Budget

| Metric | Target | Measurement Method |
|--------|--------|--------------------|
| Stack HWM | ≥ 25% of 4096 = 1024 bytes free | `uxTaskGetStackHighWaterMark()` |
| Heap impact | `std::vector<Reading>` ~5 readings × 32 bytes = ~160 bytes per cycle | `esp_get_free_heap_size()` |
| UART buffers | 256 TX + 256 RX = 512 bytes (allocated by driver) | Fixed at init |
