#!/usr/bin/env python3
"""Standalone ES-W302 reader that reuses smartfires_edge anemometer logic."""

import argparse
import time
from pathlib import Path

import sys

EDGE_RECEIVER_SRC = (
    Path(__file__).resolve().parent / "edge-receiver" / "src"
)
if str(EDGE_RECEIVER_SRC) not in sys.path:
    sys.path.insert(0, str(EDGE_RECEIVER_SRC))

from smartfires_edge.anemometer import (  # noqa: E402
    DEFAULT_ADDRESS,
    DEFAULT_BAUD,
    make_instrument,
    read_once,
)

PORT    = "/dev/cu.usbserial-BG01PRCL"
BAUD    = DEFAULT_BAUD
ADDRESS = DEFAULT_ADDRESS


def main():
    parser = argparse.ArgumentParser(description="Read ES-W302 anemometer")
    parser.add_argument("--port",    default=PORT,    help="Serial port")
    parser.add_argument("--baud",    default=BAUD,    type=int)
    parser.add_argument("--address", default=ADDRESS, type=int)
    parser.add_argument("--interval",default=1.0,     type=float, help="Poll interval seconds")
    parser.add_argument("--debug",   action="store_true", help="Show raw Modbus frames")
    args = parser.parse_args()

    inst = make_instrument(args.port, args.baud, args.address)
    inst.debug = args.debug

    print(f"ES-W302  {args.port}  {args.baud} baud  8E1  addr {args.address}")
    print(f"{'Time':>10}  {'Speed (m/s)':>12}  {'Direction (°)':>14}")
    print("-" * 42)

    errors = 0
    while True:
        try:
            speed, direction = read_once(inst)
            errors = 0
            ts = time.strftime("%H:%M:%S")
            print(f"{ts:>10}  {speed:>12.3f}  {direction:>14}", flush=True)
        except Exception as e:
            errors += 1
            if errors <= 3:
                print(f"[warn] {e}", flush=True)
            # flush stale bytes then retry
            try:
                inst.serial.reset_input_buffer()
            except Exception:
                pass
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
