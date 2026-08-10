#include <cstdint>

#include "ModbusRtuDevice.hpp"
#include "ModbusScaling.hpp"
#include "ModbusTcpDevice.hpp"
#include "unity.h"

namespace {
// The request frame from DESIGN.md §7: "slave 1, read 6 holding registers
// starting at register 0". Its CRC-16/Modbus is 0xC8C5, transmitted low byte
// first (0xC5 0xC8).
const uint8_t kRequestFrame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x06};

// A full valid response carrying the VISION.md §7.2 inverter map:
// 400.0 V, 8.50 A, 3298 W, 1234.5 kWh, status 1.
const uint8_t kResponseBody[] = {0x01, 0x03, 0x0C, 0x0F, 0xA0, 0x03, 0x52,
                                 0x0C, 0xE2, 0x00, 0x00, 0x30, 0x39, 0x00, 0x01};
}  // namespace

TEST_CASE("crc16 matches the standard check vector", "[modbus]") {
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    // 0x4B37 is the published CRC-16/Modbus check value for "123456789".
    TEST_ASSERT_EQUAL_HEX16(0x4B37, ModbusRtuDevice::crc16(check, sizeof(check)));
}

TEST_CASE("crc16 matches a real request frame", "[modbus]") {
    TEST_ASSERT_EQUAL_HEX16(0xC8C5, ModbusRtuDevice::crc16(kRequestFrame, sizeof(kRequestFrame)));
}

TEST_CASE("crc16 over a frame plus its own CRC is zero", "[modbus]") {
    // Appending the transmitted CRC (low byte first) makes the running CRC of
    // the whole frame collapse to zero — the property a receiver relies on.
    uint8_t framed[sizeof(kRequestFrame) + 2];
    for (size_t i = 0; i < sizeof(kRequestFrame); i++) {
        framed[i] = kRequestFrame[i];
    }
    const uint16_t crc = ModbusRtuDevice::crc16(kRequestFrame, sizeof(kRequestFrame));
    framed[sizeof(kRequestFrame)] = static_cast<uint8_t>(crc & 0xFF);
    framed[sizeof(kRequestFrame) + 1] = static_cast<uint8_t>(crc >> 8);

    TEST_ASSERT_EQUAL_HEX16(0x0000, ModbusRtuDevice::crc16(framed, sizeof(framed)));
}

TEST_CASE("crc16 detects a single flipped bit", "[modbus]") {
    uint8_t corrupted[sizeof(kRequestFrame)];
    for (size_t i = 0; i < sizeof(kRequestFrame); i++) {
        corrupted[i] = kRequestFrame[i];
    }
    corrupted[3] ^= 0x01;

    TEST_ASSERT_NOT_EQUAL(ModbusRtuDevice::crc16(kRequestFrame, sizeof(kRequestFrame)),
                          ModbusRtuDevice::crc16(corrupted, sizeof(corrupted)));
}

TEST_CASE("crc16 validates the response frame from the register map", "[modbus]") {
    TEST_ASSERT_EQUAL_HEX16(0x9C4C, ModbusRtuDevice::crc16(kResponseBody, sizeof(kResponseBody)));
}

TEST_CASE("scaleValue applies the register map scaling", "[modbus]") {
    // VISION.md §7.2: voltage x0.1, current x0.01, power x1.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 400.0f, modbus::scaleValue(4000, 0.1f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.5f, modbus::scaleValue(850, 0.01f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3298.0f, modbus::scaleValue(3298, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, modbus::scaleValue(0, 0.1f));
}

TEST_CASE("scaleValue32 combines two registers big-endian", "[modbus]") {
    // Energy total lives in registers 3-4, high word first, x0.1 -> kWh.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1234.5f, modbus::scaleValue32(0, 12345, 0.1f));
    // High word set: 0x0001_0000 = 65536 raw -> 6553.6 kWh.
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 6553.6f, modbus::scaleValue32(1, 0, 0.1f));
    // Full scale must not overflow into a negative via signed arithmetic.
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 429496729.5f,
                             modbus::scaleValue32(0xFFFF, 0xFFFF, 0.1f));
}

TEST_CASE("buildRequest lays out the MBAP header and PDU", "[modbus]") {
    // "Transaction 1, unit 1, read 6 holding registers from 0" — the worked
    // example in DESIGN_TCP.md §2.
    uint8_t frame[12] = {};
    ModbusTcpDevice::buildRequest(frame, 0x0001, 0x01, 0x03, 0, 6);

    const uint8_t expected[] = {0x00, 0x01,   // transaction id
                                0x00, 0x00,   // protocol id, always zero
                                0x00, 0x06,   // length: unit + func + 4 payload
                                0x01,         // unit id
                                0x03,         // function
                                0x00, 0x00,   // start register
                                0x00, 0x06};  // register count
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof(expected));
}

TEST_CASE("buildRequest carries the transaction id big-endian", "[modbus]") {
    uint8_t frame[12] = {};
    ModbusTcpDevice::buildRequest(frame, 0xBEEF, 0x11, 0x03, 0x0102, 0x0304);

    TEST_ASSERT_EQUAL_HEX8(0xBE, frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, frame[1]);
    TEST_ASSERT_EQUAL_HEX8(0x11, frame[6]);
    TEST_ASSERT_EQUAL_HEX8(0x01, frame[8]);
    TEST_ASSERT_EQUAL_HEX8(0x02, frame[9]);
    TEST_ASSERT_EQUAL_HEX8(0x03, frame[10]);
    TEST_ASSERT_EQUAL_HEX8(0x04, frame[11]);
}

TEST_CASE("MBAP length field stays fixed for a read request", "[modbus]") {
    // The length counts the unit id plus the PDU, which is always six bytes for
    // function 0x03 regardless of how many registers are asked for.
    for (uint16_t count : {uint16_t{1}, uint16_t{6}, uint16_t{125}}) {
        uint8_t frame[12] = {};
        ModbusTcpDevice::buildRequest(frame, 7, 1, 0x03, 0, count);
        TEST_ASSERT_EQUAL_HEX8(0x00, frame[4]);
        TEST_ASSERT_EQUAL_HEX8(0x06, frame[5]);
    }
}
