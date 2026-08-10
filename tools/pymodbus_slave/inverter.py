"""Simulated solar inverter datastore, shared by the RTU and TCP slave scripts.

The register map is fixed by VISION.md section 7.2 and must stay in sync with the
ESP32 drivers in components/modbus_device:

| Register | Name          | Type        | Unit | Scale |
|----------|---------------|-------------|------|-------|
| 0        | DC Voltage    | uint16      | V    | x0.1  |
| 1        | DC Current    | uint16      | A    | x0.01 |
| 2        | AC Power      | uint16      | W    | x1    |
| 3-4      | Energy Total  | uint32 (BE) | kWh  | x0.1  |
| 5        | Device Status | uint16      | -    | -     |

Everything lives in the holding-register bank (function code 3).

Requires pymodbus ~= 3.8 (see requirements.txt). Do NOT run this on pymodbus
3.9+: ModbusSlaveContext was renamed ModbusDeviceContext and reduced to a
write-once shim with no setValues, so the periodic updates below silently stop
being possible. Details in README.md.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import math

from pymodbus.datastore import (
    ModbusSequentialDataBlock,
    ModbusServerContext,
    ModbusSlaveContext,
)

_log = logging.getLogger("inverter")

# Holding-register addresses as seen on the wire.
REG_DC_VOLTAGE = 0
REG_DC_CURRENT = 1
REG_AC_POWER = 2
REG_ENERGY_TOTAL_HI = 3
REG_ENERGY_TOTAL_LO = 4
REG_DEVICE_STATUS = 5
REG_COUNT = 6

# Holding registers are function code 3 in the pymodbus datastore API.
FC_HOLDING_REGISTERS = 3

# setValues() addresses are relative to the data block's own base, so a base of
# 0 with setValues(3, 0, ...) puts DC voltage on wire register 0. Confirmed by
# round-tripping a real client against both base 0 and base 1.
DATABLOCK_BASE = 0

# Device status enumeration (register 5).
STATUS_OFF = 0
STATUS_RUNNING = 1
STATUS_FAULT = 2

# Simulation envelope. A slow sine stands in for an irradiance curve so the
# dashboard shows visibly moving values without needing a real panel.
NOMINAL_DC_VOLTAGE_V = 400.0
DC_VOLTAGE_SWING_V = 40.0
PEAK_DC_CURRENT_A = 10.5
INVERTER_EFFICIENCY = 0.97
DEFAULT_CYCLE_S = 60.0

# 12345.6 kWh -> raw 123456 = 0x0001_E240, so the uint32 high word is non-zero
# from the first poll.
DEFAULT_ENERGY_START_KWH = 12345.6

UINT16_MAX = 0xFFFF
UINT32_MAX = 0xFFFFFFFF


def clamp_u16(value: float) -> int:
    """Saturate a scaled value into the uint16 range a register can hold."""
    return max(0, min(UINT16_MAX, int(round(value))))


def split_u32_be(value: int) -> tuple[int, int]:
    """Split a uint32 into (high word, low word) for big-endian register order."""
    clamped = max(0, min(UINT32_MAX, int(value)))
    return (clamped >> 16) & UINT16_MAX, clamped & UINT16_MAX


class InverterSim:
    """Owns the datastore and advances the simulated inverter over time."""

    def __init__(
        self,
        slave_id: int = 1,
        cycle_s: float = DEFAULT_CYCLE_S,
        fault: bool = False,
        energy_start_kwh: float = DEFAULT_ENERGY_START_KWH,
    ) -> None:
        self._slave_id = slave_id
        self._cycle_s = cycle_s
        self._fault = fault
        self._elapsed_s = 0.0
        # Seeded rather than starting at zero: a real inverter reports lifetime
        # energy, and a non-zero start exercises the uint32 high word straight
        # away. Accumulating from 0 leaves registers 3-4 all-zero for minutes,
        # which would mask a byte-order bug in the 32-bit path.
        self._energy_kwh = energy_start_kwh

        self._store = ModbusSlaveContext(
            hr=ModbusSequentialDataBlock(DATABLOCK_BASE, [0] * REG_COUNT),
        )
        # single=False so the server honours the slave address on the wire,
        # which is how the ESP32 master addresses this device.
        self.context = ModbusServerContext(slaves={slave_id: self._store}, single=False)

    @property
    def slave_id(self) -> int:
        return self._slave_id

    def _irradiance(self) -> float:
        """Normalised 0.0-1.0 output factor following a slow sine."""
        phase = 2.0 * math.pi * (self._elapsed_s / self._cycle_s)
        return 0.5 * (1.0 - math.cos(phase))

    def step(self, dt_s: float) -> dict[str, float]:
        """Advance the simulation by dt_s and write the result into the datastore."""
        self._elapsed_s += dt_s
        factor = self._irradiance()

        dc_voltage_v = NOMINAL_DC_VOLTAGE_V + DC_VOLTAGE_SWING_V * (factor - 0.5) * 2.0
        dc_current_a = PEAK_DC_CURRENT_A * factor
        ac_power_w = dc_voltage_v * dc_current_a * INVERTER_EFFICIENCY

        # Wh -> kWh over the elapsed slice.
        self._energy_kwh += ac_power_w * dt_s / 3_600_000.0

        if self._fault:
            status = STATUS_FAULT
        elif ac_power_w < 1.0:
            status = STATUS_OFF
        else:
            status = STATUS_RUNNING

        energy_hi, energy_lo = split_u32_be(round(self._energy_kwh * 10.0))
        registers = [
            clamp_u16(dc_voltage_v * 10.0),
            clamp_u16(dc_current_a * 100.0),
            clamp_u16(ac_power_w),
            energy_hi,
            energy_lo,
            status,
        ]
        self._store.setValues(FC_HOLDING_REGISTERS, REG_DC_VOLTAGE, registers)

        return {
            "dc_voltage_v": dc_voltage_v,
            "dc_current_a": dc_current_a,
            "ac_power_w": ac_power_w,
            "energy_kwh": self._energy_kwh,
            "status": status,
        }

    async def run(self, interval_s: float = 1.0) -> None:
        """Update the datastore forever, once per interval_s."""
        self.step(0.0)
        while True:
            await asyncio.sleep(interval_s)
            values = self.step(interval_s)
            _log.info(
                "V=%.1f  I=%.2f  P=%.0f W  E=%.2f kWh  status=%d",
                values["dc_voltage_v"],
                values["dc_current_a"],
                values["ac_power_w"],
                values["energy_kwh"],
                values["status"],
            )


def add_common_args(parser: argparse.ArgumentParser) -> None:
    """Register the CLI arguments shared by both slave scripts."""
    parser.add_argument(
        "--slave-id",
        type=int,
        default=1,
        help="Modbus slave address this device answers to (default: 1)",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=1.0,
        help="Seconds between simulation updates (default: 1.0)",
    )
    parser.add_argument(
        "--cycle",
        type=float,
        default=DEFAULT_CYCLE_S,
        help="Seconds for one full simulated irradiance cycle (default: 60)",
    )
    parser.add_argument(
        "--energy-start",
        type=float,
        default=DEFAULT_ENERGY_START_KWH,
        help="Initial lifetime energy in kWh (default: 12345.6)",
    )
    parser.add_argument(
        "--fault",
        action="store_true",
        help="Pin register 5 to 2 (fault) to exercise the master's handling",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Enable pymodbus protocol-level debug logging",
    )


def configure_logging(verbose: bool) -> None:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    logging.getLogger("pymodbus").setLevel(logging.DEBUG if verbose else logging.WARNING)
