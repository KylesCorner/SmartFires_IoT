#!/usr/bin/env python3
"""ES-W302 anemometer reader via USB-RS485.

Confirmed Modbus RTU settings:
  Port    : /dev/cu.usbserial-BG01PRCL
  Address : 1
  Baud    : 9600
  Parity  : Even
  Stop    : 1
  Register 0x00 : wind speed    (raw ÷ 10 → m/s, 0.1 m/s resolution)
  Register 0x01 : wind direction (degrees, 0–360)

NOTE: The wind speed register on this unit is stuck at 2 (0.2 m/s)
regardless of actual wind conditions. Direction works correctly.
This is a hardware/firmware defect — the unit should be replaced.
"""

import argparse
import time
import minimalmodbus

PORT    = "/dev/cu.usbserial-BG01PRCL"
BAUD    = 9600
ADDRESS = 1


def make_instrument(port, baud, address):
    inst = minimalmodbus.Instrument(port, address, debug=False)
    inst.serial.baudrate = baud
    inst.serial.bytesize = 8
    inst.serial.parity   = minimalmodbus.serial.PARITY_EVEN
    inst.serial.stopbits = 1
    inst.serial.timeout  = 1.0
    inst.mode = minimalmodbus.MODE_RTU
    return inst


def read_once(inst):
    regs = inst.read_registers(0, 2, functioncode=3)
    return regs[0] / 10.0, regs[1]


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
            print(f"{ts:>10}  {speed:>12.1f}  {direction:>14}", flush=True)
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
