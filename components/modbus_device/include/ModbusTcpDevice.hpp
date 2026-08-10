#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "IModbusDevice.hpp"

// Modbus TCP master over Wi-Fi (DESIGN_TCP.md §3).
//
// Same IModbusDevice contract as ModbusRtuDevice, but framed with an MBAP
// header instead of a slave address and CRC, since TCP already guarantees
// ordered, error-checked delivery.
//
// RAII: the constructor only stores configuration, init() opens the socket, and
// the destructor closes it (DESIGN_TCP.md §4). The socket is reopened lazily by
// readRegisters() whenever it has been torn down, so a slave that goes away and
// comes back recovers without restarting the task.
class ModbusTcpDevice : public IModbusDevice {
public:
    ModbusTcpDevice(std::string ip_addr, uint16_t port, uint8_t unit_id,
                    std::vector<RegisterDef> register_map);
    ~ModbusTcpDevice() override;

    ModbusTcpDevice(const ModbusTcpDevice&) = delete;
    ModbusTcpDevice& operator=(const ModbusTcpDevice&) = delete;
    ModbusTcpDevice(ModbusTcpDevice&&) = delete;
    ModbusTcpDevice& operator=(ModbusTcpDevice&&) = delete;

    esp_err_t            init() override;
    std::vector<Reading> readRegisters() override;
    [[nodiscard]] std::string_view name() const override { return "modbus_tcp"; }

    // Builds the 12-byte MBAP request frame. Static and public so the framing
    // can be unit tested without a socket, mirroring how ModbusRtuDevice
    // exposes crc16 and its scale helpers.
    static void buildRequest(uint8_t* out, uint16_t transaction_id, uint8_t unit_id,
                             uint8_t func, uint16_t start_reg, uint16_t count);

private:
    esp_err_t connectSocket();
    void      closeSocket();

    // Reads exactly len bytes, looping over short reads. recv() on a stream
    // socket may return fewer bytes than asked for; DESIGN_TCP.md §5 issues a
    // single recv and assumes the whole reply arrives at once.
    esp_err_t recvExact(uint8_t* buf, size_t len) const;

    esp_err_t sendRequest(uint8_t func, uint16_t start_reg, uint16_t count,
                          uint8_t* response, size_t response_capacity, size_t* resp_len);

    [[nodiscard]] std::vector<Reading> allFailed(int64_t now) const;

    std::string              ip_addr_;
    uint16_t                 port_;
    uint8_t                  unit_id_;
    std::vector<RegisterDef> register_map_;

    int      sock_{-1};
    uint16_t transaction_id_{0};

    // Span actually requested, derived from the map in the constructor so the
    // request stays in sync with it.
    uint16_t start_reg_{0};
    uint16_t reg_span_{0};
};
