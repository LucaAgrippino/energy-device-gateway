#!/usr/bin/env python3
"""Modbus RTU slave simulating a solar inverter over RS-485.

Runs on the Raspberry Pi with the USB-to-RS-485 (CH340) adapter, answering the
ESP32's ModbusRtuDevice master.

Defaults match components/modbus_device/DESIGN.md and its Kconfig:
/dev/ttyUSB0, 9600 baud, 8N1, slave address 1.

    ./rtu_slave.py                       # defaults
    ./rtu_slave.py --port /dev/ttyUSB1   # different adapter
    ./rtu_slave.py --verbose             # log every frame
"""

from __future__ import annotations

import argparse
import asyncio
import glob
import logging
import sys

import serial
from inverter import InverterSim, add_common_args, configure_logging
from pymodbus import FramerType
from pymodbus.server import StartAsyncSerialServer

_log = logging.getLogger("rtu_slave")


def check_port(port: str) -> None:
    """Fail loudly if the serial port can't be opened.

    StartAsyncSerialServer only logs a warning when the port is missing and then
    keeps running, so the simulation loop carries on printing plausible values
    while nothing is served. That looks like a working slave and is a genuinely
    misleading way to lose an afternoon, hence this pre-flight check.
    """
    try:
        serial.Serial(port).close()
    except serial.SerialException as exc:
        available = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
        _log.error("cannot open %s: %s", port, exc)
        if available:
            _log.error("available serial ports: %s", ", ".join(available))
        else:
            _log.error(
                "no /dev/ttyUSB* or /dev/ttyACM* found — is the USB-RS-485 adapter "
                "plugged in? check `lsusb` and `dmesg | tail`"
            )
        sys.exit(1)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--port",
        default="/dev/ttyUSB0",
        help="Serial device of the USB-to-RS-485 adapter (default: /dev/ttyUSB0)",
    )
    parser.add_argument(
        "--baudrate",
        type=int,
        default=9600,
        help="Baud rate, must match CONFIG_MODBUS_RTU_BAUD_RATE (default: 9600)",
    )
    parser.add_argument(
        "--parity",
        default="N",
        choices=["N", "E", "O"],
        help="Parity, must match the ESP32 UART config (default: N)",
    )
    parser.add_argument(
        "--stopbits",
        type=int,
        default=1,
        choices=[1, 2],
        help="Stop bits (default: 1)",
    )
    add_common_args(parser)
    return parser.parse_args()


async def main() -> None:
    args = parse_args()
    configure_logging(args.verbose)
    check_port(args.port)

    sim = InverterSim(
        slave_id=args.slave_id,
        cycle_s=args.cycle,
        fault=args.fault,
        energy_start_kwh=args.energy_start,
    )

    _log.info(
        "RTU slave on %s @ %d baud 8%s%d, slave address %d",
        args.port,
        args.baudrate,
        args.parity,
        args.stopbits,
        args.slave_id,
    )

    # The simulation runs as a task on the server's event loop rather than in a
    # daemon thread, so datastore writes can't race the request handler.
    updater = asyncio.create_task(sim.run(args.interval))
    try:
        await StartAsyncSerialServer(
            sim.context,
            framer=FramerType.RTU,
            port=args.port,
            baudrate=args.baudrate,
            bytesize=8,
            parity=args.parity,
            stopbits=args.stopbits,
        )
    finally:
        updater.cancel()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        _log.info("stopped")
