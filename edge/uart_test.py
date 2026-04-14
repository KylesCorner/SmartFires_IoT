#!/usr/bin/env python3
"""
UART raw byte dump — diagnostic tool.
Prints every byte received on the serial port as hex.
Use this to verify the Feather base station is sending data to the Jetson.

Usage:
    python3 uart_test.py [--port /dev/ttyTHS1] [--baud 115200]

If bytes print  -> data is arriving, check receiver.py framing
If nothing prints after 10s -> wiring problem (swap TX/RX, check GND)
"""

import argparse
import serial
import sys

def parse_args():
    p = argparse.ArgumentParser(description="UART raw byte dump")
    p.add_argument("--port", default="/dev/ttyTHS1")
    p.add_argument("--baud", type=int, default=115200)
    return p.parse_args()

def run(port, baud):
    print(f"Opening {port} @ {baud} baud — waiting for bytes (Ctrl-C to stop)...\n")
    with serial.Serial(port, baud, timeout=5) as s:
        count = 0
        while True:
            b = s.read(1)
            if not b:
                print("(no data in 5s)", flush=True)
                continue
            print(f'{b[0]:02X}', end=' ', flush=True)
            count += 1
            if count % 16 == 0:
                print()

if __name__ == "__main__":
    args = parse_args()
    try:
        run(args.port, args.baud)
    except KeyboardInterrupt:
        print("\nDone.")
    except serial.SerialException as e:
        print(f"Serial error: {e}", file=sys.stderr)
        sys.exit(1)
