#!/usr/bin/env python3
"""
UART loopback test.
Short pin 8 (TX) to pin 10 (RX) on the Jetson 40-pin header with a jumper wire,
then run this script. If bytes come back, the Jetson UART hardware is working.

Usage:
    python3 uart_loopback.py [--port /dev/ttyTHS1]
"""

import argparse
import serial
import time
import sys

TEST_BYTES = bytes([0xAA, 0x55, 0x01, 0x02, 0x03, 0xFF])

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--port", default="/dev/ttyTHS1")
    p.add_argument("--baud", type=int, default=115200)
    return p.parse_args()

def run(port, baud):
    print(f"Opening {port} @ {baud}...")
    with serial.Serial(port, baud, timeout=1) as s:
        s.reset_input_buffer()

        print(f"Sending: {' '.join(f'{b:02X}' for b in TEST_BYTES)}")
        s.write(TEST_BYTES)

        time.sleep(0.1)
        received = s.read(len(TEST_BYTES))

        if received == TEST_BYTES:
            print("LOOPBACK OK — Jetson UART is working")
        elif received:
            print(f"Partial/wrong data: {' '.join(f'{b:02X}' for b in received)}")
            print("UART may be working but check baud rate or wiring")
        else:
            print("NO DATA received — UART pins not active or TX/RX not shorted")
            print("Make sure pin 8 and pin 10 are jumpered together")

if __name__ == "__main__":
    args = parse_args()
    try:
        run(args.port, args.baud)
    except serial.SerialException as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
