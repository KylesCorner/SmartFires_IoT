#!/usr/bin/env python3
"""ES-W302 anemometer reader via USB-RS485.

Sentec ModBus-RTU V1.11 register map:
  Port    : /dev/cu.usbserial-BG01PRCL
  Address : 1
  Baud    : 9600
  Parity  : Even
  Stop    : 1

  0x0000 (Reg 1) : Device State  — bitmask (capability flags, NOT wind speed)
  0x0001 (Reg 2) : Wind Direction — integer, degrees 0–360
  0x0002 (Reg 3) : Wind Speed hi-word  ┐ 32-bit IEEE754 float, word-swapped
  0x0003 (Reg 4) : Wind Speed lo-word  ┘ decode: unpack('>f', pack('>HH', reg4, reg3))
"""

import argparse
import struct
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
    regs = inst.read_registers(0, 4, functioncode=3)
    direction = regs[1]
    # Word-swapped IEEE754: reg3 (0x0002) is hi-word, reg4 (0x0003) is lo-word
    speed = struct.unpack('>f', struct.pack('>HH', regs[3], regs[2]))[0]
    return speed, direction


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
