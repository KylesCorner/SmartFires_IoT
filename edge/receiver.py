#!/usr/bin/env python3
"""
SmartFires edge receiver.

Reads binary telemetry frames from the Feather base station over UART
and writes sensor data to a CSV file. One row per received packet.

Usage
-----
    python receiver.py [--port /dev/ttyTHS0] [--baud 115200] [--output telemetry.csv]

Jetson UART setup
-----------------
The Feather M0 base station connects to the Jetson Orin Nano 40-pin header.

1. Use the jetson-io tool to enable the desired UART:
       sudo /opt/nvidia/jetson-io/jetson-io.py

2. Disable the serial console on that port so it is free for our use:
       sudo systemctl stop  nvgetty
       sudo systemctl disable nvgetty
       sudo udevadm trigger

3. Common device paths: /dev/ttyTHS0, /dev/ttyTHS1  (check dmesg after step 1).

Frame format received from Feather base station
-----------------------------------------------
    [0xAA][0x55][len=32: u8][rssi: i8][PktHeader: 4 bytes][FullStatePayload: 27 bytes][crc8]
    CRC-8/MAXIM covers the len byte + all 32 data bytes.
"""

import argparse
import csv
import datetime
import os
import struct
import sys

import serial

from packet import (
    BASE_FRAME_DATA_LEN,
    FRAME_M0,
    FRAME_M1,
    LORA_PAYLOAD_SIZE,
    crc8,
    decode_full_state,
)

# ---------- CSV columns ----------

CSV_COLUMNS = [
    "timestamp",        # ISO-8601 UTC at time of Jetson receipt
    "node_id",
    "seq",              # 8-bit rolling sequence number
    "session_time_ms",  # ms from node millis() — will become synced time in Phase 2
    "uptime_ms",
    "sensor_flags",
    "flame",
    "wind_mps",
    "temp_c",
    "humidity_pct",
    "lidar_cm",
    "lat",
    "lon",
    "rssi",             # dBm as seen by the base station Feather
]


# ---------- frame receiver state machine ----------

_ST_WAIT_M0   = 0
_ST_WAIT_M1   = 1
_ST_WAIT_LEN  = 2
_ST_READ_DATA = 3
_ST_CHECK_CRC = 4


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="SmartFires telemetry receiver")
    p.add_argument("--port",   default="/dev/ttyTHS1", help="Serial port (default: /dev/ttyTHS1)")
    p.add_argument("--baud",   type=int, default=115200, help="Baud rate (default: 115200)")
    p.add_argument("--output", default="telemetry.csv", help="Output CSV file (default: telemetry.csv)")
    return p.parse_args()


def run(port: str, baud: int, output_path: str) -> None:
    file_exists = os.path.isfile(output_path)

    with serial.Serial(port, baud, timeout=1) as ser, \
         open(output_path, "a", newline="") as csvfile:

        writer = csv.DictWriter(csvfile, fieldnames=CSV_COLUMNS)
        if not file_exists:
            writer.writeheader()
            csvfile.flush()

        print(f"SmartFires receiver  |  {port} @ {baud} baud  |  output: {output_path}")
        print("Waiting for packets …  (Ctrl-C to stop)\n")

        # State machine variables
        state        = _ST_WAIT_M0
        buf          = bytearray()  # accumulates [len_byte, data...]
        expected_len = 0

        while True:
            raw = ser.read(1)
            if not raw:
                continue
            b = raw[0]

            # ------------------------------------------------------------------
            if state == _ST_WAIT_M0:
                if b == FRAME_M0:
                    state = _ST_WAIT_M1

            # ------------------------------------------------------------------
            elif state == _ST_WAIT_M1:
                state = _ST_WAIT_LEN if b == FRAME_M1 else _ST_WAIT_M0

            # ------------------------------------------------------------------
            elif state == _ST_WAIT_LEN:
                expected_len = b
                if expected_len != BASE_FRAME_DATA_LEN:
                    # Unexpected length — not our frame type; resync
                    print(
                        f"[WARN] unexpected frame len={expected_len} "
                        f"(expected {BASE_FRAME_DATA_LEN}), resyncing",
                        file=sys.stderr,
                    )
                    state = _ST_WAIT_M0
                else:
                    buf = bytearray([b])   # buf[0] = len byte (included in CRC)
                    state = _ST_READ_DATA

            # ------------------------------------------------------------------
            elif state == _ST_READ_DATA:
                buf.append(b)
                # buf holds [len_byte, data[0..expected_len-1]]
                if len(buf) == 1 + expected_len:
                    state = _ST_CHECK_CRC

            # ------------------------------------------------------------------
            elif state == _ST_CHECK_CRC:
                received_crc = b
                computed_crc = crc8(bytes(buf))

                if received_crc == computed_crc:
                    # buf[0]  = len byte
                    # buf[1]  = rssi (signed)
                    # buf[2:] = raw LoRa payload (PktHeader + FullStatePayload)
                    rssi        = struct.unpack_from("<b", buf, 1)[0]
                    raw_payload = bytes(buf[2 : 2 + LORA_PAYLOAD_SIZE])

                    pkt = decode_full_state(raw_payload, rssi)
                    if pkt is not None:
                        pkt["timestamp"] = datetime.datetime.utcnow().isoformat(
                            timespec="milliseconds"
                        )
                        writer.writerow(pkt)
                        csvfile.flush()
                        print(
                            f"[RX] node={pkt['node_id']}  seq={pkt['seq']:3d}  "
                            f"T={pkt['temp_c']:5.1f}°C  H={pkt['humidity_pct']:4.1f}%  "
                            f"wind={pkt['wind_mps']:.2f} m/s  "
                            f"flame={'YES' if pkt['flame'] else 'no '}  "
                            f"rssi={rssi:4d} dBm"
                        )
                    else:
                        print(
                            f"[WARN] packet decode failed (payload len={len(raw_payload)})",
                            file=sys.stderr,
                        )
                else:
                    print(
                        f"[WARN] CRC mismatch: got {received_crc:#04x}, "
                        f"expected {computed_crc:#04x}",
                        file=sys.stderr,
                    )

                # Always reset after CRC check
                state = _ST_WAIT_M0
                buf   = bytearray()


# ---------- entry point ----------

if __name__ == "__main__":
    args = _parse_args()
    try:
        run(args.port, args.baud, args.output)
    except KeyboardInterrupt:
        print("\nReceiver stopped.")
    except serial.SerialException as exc:
        print(f"Serial error: {exc}", file=sys.stderr)
        print(
            "Tip: check --port argument and run  ls /dev/ttyTHS*  on the Jetson.",
            file=sys.stderr,
        )
        sys.exit(1)
