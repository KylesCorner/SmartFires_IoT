#!/usr/bin/env python3
"""
SmartFires edge receiver.

Reads binary telemetry frames from the Feather base station over UART
and writes sensor data to a CSV file. One row per received packet.

Also sends periodic TIME_SYNC frames to the base station so all nodes
can maintain a Jetson-synced session clock. The session starts when this
process starts; session_time_ms is ms elapsed since then.

Usage
-----
    python receiver.py [--port /dev/ttyTHS0] [--baud 115200] [--output telemetry.csv]
                       [--sync-interval 30]

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
    [0xAA][0x55][len: u8][rssi: i8][LoRa payload][crc8]
    len = 1 (rssi) + lora_payload_len
      FULL_STATE: len=37  (rssi + PktHeader:4 + FullStatePayload:32)
      BUNDLE:     len=178 max (rssi + PktHeader:4 + FullStatePayload:32 + n_deltas:1 + 7×DeltaPayload:140)
    CRC-8/MAXIM covers the len byte + all data bytes that follow.

TIME_SYNC frame sent to Feather base station
--------------------------------------------
    [0xAA][0x55][len=12: u8][PktHeader(PKT_TIME_SYNC): 4 bytes][TimeSyncPayload: 8 bytes][crc8]
    The base station broadcasts this over LoRa so all nodes update their session clock.
"""

import argparse
import csv
import datetime
import os
import random
import struct
import sys
import threading
import time

import serial

from packet import (
    BASE_FRAME_MIN_DATA_LEN,
    BASE_FRAME_MAX_DATA_LEN,
    FRAME_M0,
    FRAME_M1,
    HEADER_FMT,
    PKT_MAGIC,
    PKT_FULL_STATE,
    PKT_BUNDLE,
    crc8,
    decode_full_state,
    decode_bundle,
    encode_time_sync_frame,
)

# ---------- CSV columns ----------

CSV_COLUMNS = [
    "timestamp",        # ISO-8601 UTC wall-clock at time of Jetson receipt
    "node_id",
    "seq",              # 8-bit rolling sequence number
    "session_time_ms",  # synced ms since session start (Jetson-derived after first TIME_SYNC)
    "uptime_ms",        # local ESP32 millis()
    "sensor_flags",
    "wind_mps",
    "temp_c",
    "humidity_pct",
    "pm1_0_ug_m3",
    "pm2_5_ug_m3",
    "pm4_0_ug_m3",
    "pm10_ug_m3",
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
    p.add_argument("--port",          default="/dev/ttyTHS1", help="Serial port (default: /dev/ttyTHS1)")
    p.add_argument("--baud",          type=int, default=115200, help="Baud rate (default: 115200)")
    p.add_argument("--output",        default="telemetry.csv", help="Output CSV file (default: telemetry.csv)")
    p.add_argument("--sync-interval", type=int, default=30,    help="TIME_SYNC interval in seconds (default: 30)")
    return p.parse_args()


# ---------- TIME_SYNC sender thread ----------

def _time_sync_sender(
    ser: serial.Serial,
    write_lock: threading.Lock,
    session_id: int,
    session_start: float,
    interval_s: int,
) -> None:
    """Background thread: sends a TIME_SYNC frame to the base Feather every interval_s seconds."""
    seq = 0
    while True:
        time.sleep(interval_s)
        session_ms = int((time.time() - session_start) * 1000) & 0xFFFFFFFF
        frame = encode_time_sync_frame(session_id, session_ms, seq)
        with write_lock:
            try:
                ser.write(frame)
            except serial.SerialException as exc:
                print(f"[TIME_SYNC] write error: {exc}", file=sys.stderr)
        print(f"[TIME_SYNC] sent  session_id={session_id:#010x}  session_ms={session_ms}")
        seq = (seq + 1) & 0xFF


# ---------- main receive loop ----------

def run(port: str, baud: int, output_path: str, sync_interval_s: int) -> None:
    file_exists = os.path.isfile(output_path)

    # Session identity: changes each time receiver.py restarts so nodes reset their offsets.
    session_id    = random.randint(1, 0xFFFFFFFF)
    session_start = time.time()

    print(f"SmartFires receiver  |  {port} @ {baud} baud  |  output: {output_path}")
    print(f"Session ID: {session_id:#010x}  |  TIME_SYNC every {sync_interval_s}s")
    print("Waiting for packets …  (Ctrl-C to stop)\n")

    with serial.Serial(port, baud, timeout=1) as ser, \
         open(output_path, "a", newline="") as csvfile:

        writer = csv.DictWriter(csvfile, fieldnames=CSV_COLUMNS)
        if not file_exists:
            writer.writeheader()
            csvfile.flush()

        write_lock = threading.Lock()

        # Send an immediate TIME_SYNC so nodes sync as soon as possible after startup.
        initial_frame = encode_time_sync_frame(session_id, 0, seq=0)
        with write_lock:
            ser.write(initial_frame)
        print(f"[TIME_SYNC] initial sync sent")

        # Background thread sends TIME_SYNC every sync_interval_s seconds.
        sync_thread = threading.Thread(
            target=_time_sync_sender,
            args=(ser, write_lock, session_id, session_start, sync_interval_s),
            daemon=True,
        )
        sync_thread.start()

        # State machine variables
        state        = _ST_WAIT_M0
        buf          = bytearray()
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
                if expected_len < BASE_FRAME_MIN_DATA_LEN or expected_len > BASE_FRAME_MAX_DATA_LEN:
                    print(
                        f"[WARN] unexpected frame len={expected_len} "
                        f"(expected {BASE_FRAME_MIN_DATA_LEN}–{BASE_FRAME_MAX_DATA_LEN}), resyncing",
                        file=sys.stderr,
                    )
                    state = _ST_WAIT_M0
                else:
                    buf = bytearray([b])   # buf[0] = len byte (included in CRC)
                    state = _ST_READ_DATA

            # ------------------------------------------------------------------
            elif state == _ST_READ_DATA:
                buf.append(b)
                if len(buf) == 1 + expected_len:
                    state = _ST_CHECK_CRC

            # ------------------------------------------------------------------
            elif state == _ST_CHECK_CRC:
                received_crc = b
                computed_crc = crc8(bytes(buf))

                if received_crc == computed_crc:
                    # buf[0]  = len byte
                    # buf[1]  = rssi (signed)
                    # buf[2:] = raw LoRa payload (variable length)
                    rssi        = struct.unpack_from("<b", buf, 1)[0]
                    raw_payload = bytes(buf[2:])

                    pkt_type = raw_payload[1] if len(raw_payload) >= 2 else 0xFF

                    if pkt_type == PKT_FULL_STATE:
                        pkts = [decode_full_state(raw_payload, rssi)]
                    elif pkt_type == PKT_BUNDLE:
                        pkts = decode_bundle(raw_payload, rssi)
                    else:
                        pkts = []
                        print(
                            f"[WARN] unknown pkt_type=0x{pkt_type:02x}",
                            file=sys.stderr,
                        )

                    now_ts = datetime.datetime.utcnow().isoformat(timespec="milliseconds")
                    wrote = 0
                    for pkt in pkts:
                        if pkt is None:
                            continue
                        pkt["timestamp"] = now_ts
                        writer.writerow(pkt)
                        wrote += 1

                    if wrote > 0:
                        csvfile.flush()
                        first = pkts[0]
                        print(
                            f"[RX] node={first['node_id']}  seq={first['seq']:3d}  "
                            f"records={wrote}  "
                            f"session={first['session_time_ms']}ms  "
                            f"T={first['temp_c']:5.1f}°C  H={first['humidity_pct']:4.1f}%  "
                            f"wind={first['wind_mps']:.2f} m/s  "
                            f"PM2.5={first['pm2_5_ug_m3']:.1f} PM10={first['pm10_ug_m3']:.1f} µg/m³  "
                            f"lat={first['lat']:.5f} lon={first['lon']:.5f}  "
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

                state = _ST_WAIT_M0
                buf   = bytearray()


# ---------- entry point ----------

if __name__ == "__main__":
    args = _parse_args()
    try:
        run(args.port, args.baud, args.output, args.sync_interval)
    except KeyboardInterrupt:
        print("\nReceiver stopped.")
    except serial.SerialException as exc:
        print(f"Serial error: {exc}", file=sys.stderr)
        print(
            "Tip: check --port argument and run  ls /dev/ttyTHS*  on the Jetson.",
            file=sys.stderr,
        )
        sys.exit(1)
