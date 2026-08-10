#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "IModbusDevice.hpp"
#include "driver/gpio.h"
#include "driver/uart.h"

// Modbus RTU master over RS-485 (DESIGN.md §4).
//
// RAII: the constructor only stores configuration, init() installs the UART
// driver, and the destructor deletes it (DESIGN.md §5).
class ModbusRtuDevice : public IModbusDevice {
public:
    // baud_rate is a constructor parameter rather than the hard-coded 9600 of
    // DESIGN.md §5, so that CONFIG_MODBUS_RTU_BAUD_RATE (DESIGN.md §11) is
    // actually honoured — the design specifies both and they disagreed.
    ModbusRtuDevice(uart_port_t port, uint8_t slave_addr, uint32_t baud_rate,
                    gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t de_re_pin,
                    std::vector<RegisterDef> register_map);
    ~ModbusRtuDevice() override;

    ModbusRtuDevice(const ModbusRtuDevice&) = delete;
    ModbusRtuDevice& operator=(const ModbusRtuDevice&) = delete;
    ModbusRtuDevice(ModbusRtuDevice&&) = delete;
    ModbusRtuDevice& operator=(ModbusRtuDevice&&) = delete;

    esp_err_t            init() override;
    std::vector<Reading> readRegisters() override;
    [[nodiscard]] std::string_view name() const override { return "modbus_rtu"; }

    // Pure helpers, static and public so the unit tests can exercise them
    // without any UART hardware — same approach as Mpu9150's scale* methods.
    static uint16_t crc16(const uint8_t* data, size_t len);
    static float    scaleValue(uint16_t raw, float scale);
    static float    scaleValue32(uint16_t high, uint16_t low, float scale);

private:
    // Sends one request and validates the reply: slave address, function code,
    // slave exception bit, declared byte count and CRC (DESIGN.md §12).
    esp_err_t sendRequest(uint8_t func, uint16_t start_reg, uint16_t count,
                          uint8_t* response, size_t response_capacity, size_t* resp_len);

    // Every register reported as ERROR, used whenever the exchange fails.
    [[nodiscard]] std::vector<Reading> allFailed(int64_t now) const;

    uart_port_t              port_;
    uint8_t                  slave_addr_;
    uint32_t                 baud_rate_;
    gpio_num_t               tx_pin_;
    gpio_num_t               rx_pin_;
    gpio_num_t               de_re_pin_;
    std::vector<RegisterDef> register_map_;

    // Span actually requested, derived from the map in the constructor so the
    // request stays in sync with it instead of the fixed "registers 0-5" of
    // DESIGN.md §8.
    uint16_t start_reg_{0};
    uint16_t reg_span_{0};

    bool driver_installed_{false};
};
