#include "ModbusTcpDevice.hpp"

#include "ModbusScaling.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace {
constexpr const char* kTag = "ModbusTcp";

constexpr uint8_t kFuncReadHoldingRegisters = 0x03;
constexpr uint8_t kExceptionBit = 0x80;

// [tid:2][protocol:2][length:2][unit:1] — DESIGN_TCP.md §2.
constexpr size_t kMbapHeaderBytes = 7;
// The MBAP length field counts the unit id and everything after it.
constexpr size_t kMbapLengthOffset = 4;
constexpr size_t kRequestBytes = 12;

// byte_count is a single octet, so a well-formed reply cannot exceed this.
constexpr size_t kMaxResponseBytes = kMbapHeaderBytes + 2 + 255;

constexpr int kSocketTimeoutMs = CONFIG_MODBUS_TCP_TIMEOUT_MS;

uint16_t beU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
}  // namespace

ModbusTcpDevice::ModbusTcpDevice(std::string ip_addr, uint16_t port, uint8_t unit_id,
                                 std::vector<RegisterDef> register_map)
    : ip_addr_(std::move(ip_addr)),
      port_(port),
      unit_id_(unit_id),
      register_map_(std::move(register_map)) {
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

ModbusTcpDevice::~ModbusTcpDevice() {
    closeSocket();
}

esp_err_t ModbusTcpDevice::init() {
    if (register_map_.empty()) {
        ESP_LOGE(kTag, "register map is empty, nothing to poll");
        return ESP_ERR_INVALID_ARG;
    }
    return connectSocket();
}

esp_err_t ModbusTcpDevice::connectSocket() {
    closeSocket();

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port_);
    if (inet_pton(AF_INET, ip_addr_.c_str(), &dest.sin_addr) != 1) {
        ESP_LOGE(kTag, "'%s' is not a valid IPv4 address", ip_addr_.c_str());
        return ESP_ERR_INVALID_ARG;
    }

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ < 0) {
        ESP_LOGE(kTag, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    // Bound both directions: without a send timeout a full transmit window on a
    // dead peer would block the poll task indefinitely.
    timeval tv{};
    tv.tv_sec = kSocketTimeoutMs / 1000;
    tv.tv_usec = (kSocketTimeoutMs % 1000) * 1000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock_, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) != 0) {
        ESP_LOGW(kTag, "connect to %s:%u failed: errno %d", ip_addr_.c_str(), port_, errno);
        closeSocket();
        return ESP_FAIL;
    }

    ESP_LOGI(kTag, "connected to Modbus TCP slave at %s:%u (unit %u, regs %u..%u)",
             ip_addr_.c_str(), port_, unit_id_, start_reg_,
             static_cast<unsigned>(start_reg_ + reg_span_ - 1));
    return ESP_OK;
}

void ModbusTcpDevice::closeSocket() {
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}

void ModbusTcpDevice::buildRequest(uint8_t* out, uint16_t transaction_id, uint8_t unit_id,
                                   uint8_t func, uint16_t start_reg, uint16_t count) {
    out[0] = static_cast<uint8_t>(transaction_id >> 8);
    out[1] = static_cast<uint8_t>(transaction_id & 0xFF);
    out[2] = 0x00;  // protocol id, always zero for Modbus
    out[3] = 0x00;
    out[4] = 0x00;  // length: unit id + function + four payload bytes
    out[5] = 0x06;
    out[6] = unit_id;
    out[7] = func;
    out[8] = static_cast<uint8_t>(start_reg >> 8);
    out[9] = static_cast<uint8_t>(start_reg & 0xFF);
    out[10] = static_cast<uint8_t>(count >> 8);
    out[11] = static_cast<uint8_t>(count & 0xFF);
}

esp_err_t ModbusTcpDevice::recvExact(uint8_t* buf, size_t len) const {
    size_t got = 0;
    while (got < len) {
        const int n = recv(sock_, buf + got, len - got, 0);
        if (n == 0) {
            ESP_LOGW(kTag, "slave closed the connection");
            return ESP_ERR_INVALID_STATE;
        }
        if (n < 0) {
            ESP_LOGW(kTag, "recv failed after %u of %u bytes: errno %d",
                     static_cast<unsigned>(got), static_cast<unsigned>(len), errno);
            return ESP_ERR_TIMEOUT;
        }
        got += static_cast<size_t>(n);
    }
    return ESP_OK;
}

esp_err_t ModbusTcpDevice::sendRequest(uint8_t func, uint16_t start_reg, uint16_t count,
                                       uint8_t* response, size_t response_capacity,
                                       size_t* resp_len) {
    const uint16_t tid = transaction_id_++;

    uint8_t request[kRequestBytes];
    buildRequest(request, tid, unit_id_, func, start_reg, count);

    const int sent = send(sock_, request, sizeof(request), 0);
    if (sent != static_cast<int>(sizeof(request))) {
        ESP_LOGW(kTag, "send wrote %d of %u bytes: errno %d",
                 sent, static_cast<unsigned>(sizeof(request)), errno);
        return ESP_FAIL;
    }

    // Header first: its length field says how much of the PDU follows, so the
    // rest can be read exactly rather than guessed at.
    esp_err_t err = recvExact(response, kMbapHeaderBytes);
    if (err != ESP_OK) {
        return err;
    }

    const uint16_t resp_tid = beU16(response);
    const uint16_t protocol = beU16(response + 2);
    const uint16_t length = beU16(response + kMbapLengthOffset);

    if (protocol != 0) {
        ESP_LOGW(kTag, "protocol id %u in response, expected 0", protocol);
        return ESP_ERR_INVALID_RESPONSE;
    }
    // DESIGN_TCP.md §5/§9 only warn on a transaction id mismatch and use the
    // data anyway. That is unsafe: with one request in flight at a time, a
    // mismatch means the stream is desynchronised — typically a reply to an
    // earlier request that timed out — so the payload belongs to a different
    // question than the one just asked. Treating it as an error forces a
    // reconnect instead of decoding stale registers as current.
    if (resp_tid != tid) {
        ESP_LOGW(kTag, "transaction id mismatch: sent %u, got %u — resynchronising",
                 tid, resp_tid);
        return ESP_ERR_INVALID_RESPONSE;
    }
    // length counts the unit id plus the PDU, so at minimum unit + function.
    if (length < 2) {
        ESP_LOGW(kTag, "MBAP length %u is too small", length);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t remaining = length - 1;  // the unit id is already in the header
    if (kMbapHeaderBytes + remaining > response_capacity) {
        ESP_LOGE(kTag, "response of %u bytes exceeds the %u-byte buffer",
                 static_cast<unsigned>(kMbapHeaderBytes + remaining),
                 static_cast<unsigned>(response_capacity));
        return ESP_ERR_INVALID_SIZE;
    }

    err = recvExact(response + kMbapHeaderBytes, remaining);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t resp_func = response[kMbapHeaderBytes];
    if ((resp_func & kExceptionBit) != 0) {
        // Exception replies carry the code where the byte count would be.
        ESP_LOGE(kTag, "slave reported exception 0x%02X", response[kMbapHeaderBytes + 1]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (resp_func != func) {
        ESP_LOGW(kTag, "function code 0x%02X in response, expected 0x%02X", resp_func, func);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *resp_len = kMbapHeaderBytes + remaining;
    return ESP_OK;
}

std::vector<Reading> ModbusTcpDevice::readRegisters() {
    const int64_t now = esp_timer_get_time();

    // Lazy reconnect: the socket is torn down on any failure so the next poll
    // re-establishes it (DESIGN_TCP.md §6 "Reconnection Strategy").
    if (sock_ < 0 && connectSocket() != ESP_OK) {
        return allFailed(now);
    }

    uint8_t response[kMaxResponseBytes];
    size_t  resp_len = 0;

    esp_err_t err = sendRequest(kFuncReadHoldingRegisters, start_reg_, reg_span_,
                                response, sizeof(response), &resp_len);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "read of %u registers from %u failed: %s",
                 reg_span_, start_reg_, esp_err_to_name(err));
        closeSocket();
        return allFailed(now);
    }

    // [MBAP:7][func:1][byte_count:1][payload...]
    const size_t   byte_count = response[kMbapHeaderBytes + 1];
    const uint8_t* data = response + kMbapHeaderBytes + 2;

    std::vector<Reading> readings;
    readings.reserve(register_map_.size());

    for (const auto& reg : register_map_) {
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

        // Scaling is identical to RTU (DESIGN_TCP.md §5) — only the payload
        // offset differs, so the conversions are reused rather than duplicated.
        const float value =
            (reg.reg_count == 1)
                ? modbus::scaleValue(beU16(data + offset), reg.scale)
                : modbus::scaleValue32(beU16(data + offset),
                                       beU16(data + offset + 2), reg.scale);

        readings.emplace_back(reg.name, value, now, Reading::Status::OK);
    }

    return readings;
}

std::vector<Reading> ModbusTcpDevice::allFailed(int64_t now) const {
    std::vector<Reading> readings;
    readings.reserve(register_map_.size());
    for (const auto& reg : register_map_) {
        readings.emplace_back(reg.name, 0.0F, now, Reading::Status::ERROR);
    }
    return readings;
}
