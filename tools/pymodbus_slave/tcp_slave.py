#!/usr/bin/env python3
"""Modbus TCP slave simulating a solar inverter over the network.

Runs on the Raspberry Pi, answering the ESP32's ModbusTcpDevice master. Serves
exactly the same register map as rtu_slave.py — only the transport differs.

Defaults match components/modbus_device/Kconfig: port 5020, unit id 1.

    ./tcp_slave.py                    # defaults
    ./tcp_slave.py --port 502         # registered Modbus port, needs root
    ./tcp_slave.py --verbose          # log every frame

Port 502 is privileged, so it needs sudo. 5020 avoids that; the ESP32 side just
has to agree via CONFIG_MODBUS_TCP_PORT.
"""

from __future__ import annotations

import argparse
import asyncio
import logging

from inverter import InverterSim, add_common_args, configure_logging
from pymodbus import FramerType
from pymodbus.server import StartAsyncTcpServer

_log = logging.getLogger("tcp_slave")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="Address to bind (default: 0.0.0.0, all interfaces)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=5020,
        help="TCP port, must match CONFIG_MODBUS_TCP_PORT (default: 5020)",
    )
    add_common_args(parser)
    return parser.parse_args()


async def main() -> None:
    args = parse_args()
    configure_logging(args.verbose)

    sim = InverterSim(
        slave_id=args.slave_id,
        cycle_s=args.cycle,
        fault=args.fault,
        energy_start_kwh=args.energy_start,
    )

    _log.info("TCP slave on %s:%d, unit id %d", args.host, args.port, args.slave_id)
    if args.port < 1024:
        _log.info("port %d is privileged — this needs to run as root", args.port)

    # Same reasoning as the RTU slave: the simulation runs as a task on the
    # server's event loop, so datastore writes cannot race the request handler.
    updater = asyncio.create_task(sim.run(args.interval))
    try:
        await StartAsyncTcpServer(
            sim.context,
            framer=FramerType.SOCKET,
            address=(args.host, args.port),
        )
    finally:
        updater.cancel()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        _log.info("stopped")
