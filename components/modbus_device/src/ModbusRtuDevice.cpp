#include "ModbusRtuDevice.hpp"

#include <algorithm>
#include <cinttypes>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {
constexpr const char* kTag = "ModbusRtu";

constexpr uint8_t kFuncReadHoldingRegisters = 0x03;
constexpr uint8_t kExceptionBit = 0x80;

// [addr][func][start:2][count:2][crc:2] — DESIGN.md §7.
constexpr size_t kRequestBytes = 8;
// [addr][func][byte_count] before the payload, [crc:2] after it.
constexpr size_t kResponseHeaderBytes = 3;
constexpr size_t kCrcBytes = 2;

// A Modbus RTU response carries at most 255 payload bytes (byte_count is a
// single octet), so this bounds any well-formed reply.
constexpr size_t kMaxResponseBytes = kResponseHeaderBytes + 255 + kCrcBytes;

// The driver requires each ring buffer to exceed the 128-byte hardware FIFO.
constexpr int kUartRxBufferBytes = 256;
constexpr int kUartTxBufferBytes = 256;

constexpr TickType_t kResponseTimeout = pdMS_TO_TICKS(CONFIG_MODBUS_RTU_RESPONSE_TIMEOUT_MS);

uint16_t beU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
}  // namespace

ModbusRtuDevice::ModbusRtuDevice(uart_port_t port, uint8_t slave_addr, uint32_t baud_rate,
                                 gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t de_re_pin,
                                 std::vector<RegisterDef> register_map)
    : port_(port),
      slave_addr_(slave_addr),
      baud_rate_(baud_rate),
      tx_pin_(tx_pin),
      rx_pin_(rx_pin),
      de_re_pin_(de_re_pin),
      register_map_(std::move(register_map)) {
    // Request exactly the span the map covers, in a single transaction.
    if (!register_map_.empty()) {
        uint16_t lowest = UINT16_MAX;
        uint16_t highest_end = 0;
        for (const auto& reg : register_map_) {
            lowest = std::min(lowest, reg.address);
            highest_end = std::max(highest_end,
                                   static_cast<uint16_t>(reg.address + reg.reg_count));
        }
        start_reg_ = lowest;
        reg_span_ = static_cast<uint16_t>(highest_end - lowest);
    }
}

ModbusRtuDevice::~ModbusRtuDevice() {
    if (driver_installed_) {
        esp_err_t err = uart_driver_delete(port_);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "failed to delete UART driver: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t ModbusRtuDevice::init() {
    if (register_map_.empty()) {
        ESP_LOGE(kTag, "register map is empty, nothing to poll");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = uart_driver_install(port_, kUartRxBufferBytes, kUartTxBufferBytes,
                                        0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    driver_installed_ = true;

    uart_config_t uart_cfg{};
    uart_cfg.baud_rate = static_cast<int>(baud_rate_);
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    err = uart_param_config(port_, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // The DE/RE pin is driven as RTS; UART_MODE_RS485_HALF_DUPLEX makes the
    // driver assert it for the duration of each transmission (DESIGN.md §2).
    err = uart_set_pin(port_, tx_pin_, rx_pin_, de_re_pin_, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_mode(port_, UART_MODE_RS485_HALF_DUPLEX);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_set_mode(RS485_HALF_DUPLEX) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(kTag, "%.*s initialized (UART%d, slave %u, %" PRIu32 " baud, regs %u..%u)",
             static_cast<int>(name().size()), name().data(), static_cast<int>(port_),
             slave_addr_, baud_rate_, start_reg_,
             static_cast<unsigned>(start_reg_ + reg_span_ - 1));
    return ESP_OK;
}

std::vector<Reading> ModbusRtuDevice::readRegisters() {
    const int64_t now = esp_timer_get_time();

    uint8_t response[kMaxResponseBytes];
    size_t  resp_len = 0;

    esp_err_t err = sendRequest(kFuncReadHoldingRegisters, start_reg_, reg_span_,
                                response, sizeof(response), &resp_len);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "read of %u registers from %u failed: %s",
                 reg_span_, start_reg_, esp_err_to_name(err));
        return allFailed(now);
    }

    const size_t   byte_count = response[2];
    const uint8_t* data = response + kResponseHeaderBytes;

    std::vector<Reading> readings;
    readings.reserve(register_map_.size());

    for (const auto& reg : register_map_) {
        // Offset relative to the first register requested. DESIGN.md §8 uses
        // reg.address * 2, which silently assumes the map starts at register 0.
        const size_t offset = static_cast<size_t>(reg.address - start_reg_) * 2;
        const size_t needed = offset + (static_cast<size_t>(reg.reg_count) * 2);
        if (needed > byte_count) {
            ESP_LOGW(kTag, "%.*s needs %u bytes at offset %u but slave returned %u",
                     static_cast<int>(reg.name.size()), reg.name.data(),
                     static_cast<unsigned>(reg.reg_count * 2), static_cast<unsigned>(offset),
                     static_cast<unsigned>(byte_count));
            readings.emplace_back(reg.name, 0.0F, now, Reading::Status::ERROR);
            continue;
        }

        const float value =
            (reg.reg_count == 1)
                ? scaleValue(beU16(data + offset), reg.scale)
                : scaleValue32(beU16(data + offset), beU16(data + offset + 2), reg.scale);

        readings.emplace_back(reg.name, value, now, Reading::Status::OK);
    }

    return readings;
}

esp_err_t ModbusRtuDevice::sendRequest(uint8_t func, uint16_t start_reg, uint16_t count,
                                       uint8_t* response, size_t response_capacity,
                                       size_t* resp_len) {
    uint8_t request[kRequestBytes];
    request[0] = slave_addr_;
    request[1] = func;
    request[2] = static_cast<uint8_t>(start_reg >> 8);
    request[3] = static_cast<uint8_t>(start_reg & 0xFF);
    request[4] = static_cast<uint8_t>(count >> 8);
    request[5] = static_cast<uint8_t>(count & 0xFF);

    // Modbus RTU transmits the CRC low byte first.
    const uint16_t request_crc = crc16(request, 6);
    request[6] = static_cast<uint8_t>(request_crc & 0xFF);
    request[7] = static_cast<uint8_t>(request_crc >> 8);

    // Discard anything left in the RX buffer so a stale byte from a previous
    // exchange can't be mistaken for the head of this response.
    uart_flush_input(port_);

    const int written = uart_write_bytes(port_, request, sizeof(request));
    if (written != static_cast<int>(sizeof(request))) {
        ESP_LOGE(kTag, "uart_write_bytes wrote %d of %u bytes",
                 written, static_cast<unsigned>(sizeof(request)));
        return ESP_FAIL;
    }

    // uart_write_bytes only queues the frame. Wait for the last bit to leave the
    // shifter so DE/RE has dropped and the bus is released before listening.
    esp_err_t err = uart_wait_tx_done(port_, kResponseTimeout);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_wait_tx_done failed: %s", esp_err_to_name(err));
        return err;
    }

    // Read the header first: an exception reply is shorter than a data reply,
    // so the total length isn't known until the function code has been seen.
    const int header = uart_read_bytes(port_, response, kResponseHeaderBytes, kResponseTimeout);
    if (header != static_cast<int>(kResponseHeaderBytes)) {
        return ESP_ERR_TIMEOUT;
    }

    if (response[0] != slave_addr_) {
        ESP_LOGW(kTag, "response from slave %u, expected %u", response[0], slave_addr_);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t remaining = 0;
    const bool is_exception = (response[1] & kExceptionBit) != 0;
    if (is_exception) {
        // [addr][func|0x80][exception_code][crc:2] — the code is already in
        // response[2], only the CRC is still on the wire.
        remaining = kCrcBytes;
    } else if (response[1] != func) {
        ESP_LOGW(kTag, "function code 0x%02X in response, expected 0x%02X", response[1], func);
        return ESP_ERR_INVALID_RESPONSE;
    } else {
        remaining = static_cast<size_t>(response[2]) + kCrcBytes;
    }

    if (kResponseHeaderBytes + remaining > response_capacity) {
        ESP_LOGE(kTag, "response of %u bytes exceeds the %u-byte buffer",
                 static_cast<unsigned>(kResponseHeaderBytes + remaining),
                 static_cast<unsigned>(response_capacity));
        return ESP_ERR_INVALID_SIZE;
    }

    const int rest = uart_read_bytes(port_, response + kResponseHeaderBytes,
                                     remaining, kResponseTimeout);
    if (rest != static_cast<int>(remaining)) {
        ESP_LOGW(kTag, "partial response: got %d of %u trailing bytes",
                 rest, static_cast<unsigned>(remaining));
        return ESP_ERR_TIMEOUT;
    }

    const size_t   total = kResponseHeaderBytes + remaining;
    const uint16_t computed = crc16(response, total - kCrcBytes);
    const auto received =
        static_cast<uint16_t>(response[total - 2] | (response[total - 1] << 8));
    if (computed != received) {
        ESP_LOGW(kTag, "CRC mismatch: computed 0x%04X, received 0x%04X", computed, received);
        return ESP_ERR_INVALID_CRC;
    }

    // CRC is validated before trusting the exception code it protects.
    if (is_exception) {
        ESP_LOGE(kTag, "slave reported exception 0x%02X", response[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *resp_len = total;
    return ESP_OK;
}

std::vector<Reading> ModbusRtuDevice::allFailed(int64_t now) const {
    std::vector<Reading> readings;
    readings.reserve(register_map_.size());
    for (const auto& reg : register_map_) {
        readings.emplace_back(reg.name, 0.0F, now, Reading::Status::ERROR);
    }
    return readings;
}

uint16_t ModbusRtuDevice::crc16(const uint8_t* data, size_t len) {
    // CRC-16/Modbus: reflected, polynomial 0xA001, init 0xFFFF (DESIGN.md §7).
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
            } else {
                crc = static_cast<uint16_t>(crc >> 1);
            }
        }
    }
    return crc;
}

float ModbusRtuDevice::scaleValue(uint16_t raw, float scale) {
    return static_cast<float>(raw) * scale;
}

float ModbusRtuDevice::scaleValue32(uint16_t high, uint16_t low, float scale) {
    const uint32_t raw = (static_cast<uint32_t>(high) << 16) | low;
    return static_cast<float>(raw) * scale;
}
