#!/usr/bin/env python3
"""
SmartFires LoRa integration test for packet loss.

Purpose
-------
Listen on the Jetson UART connected to the Feather base station and measure
per-node packet loss for node 1 and node 2 using the rolling 8-bit sequence ID.

This script reuses the same framing assumptions as receiver.py:
  [0xAA][0x55][len][rssi][payload...][crc8]
and the same packet.decode_full_state() parser.

Usage
-----
python integration_test_packet_loss.py --port /dev/ttyTHS1 --baud 115200 --duration 60
python integration_test_packet_loss.py --port /dev/ttyTHS1 --duration 120 --nodes 1 2

Notes
-----
- Loss is inferred from gaps in the rolling 8-bit seq field.
- First packet from each node establishes the baseline and is not counted as loss.
- Duplicate packets are counted separately.
- Out-of-order packets are counted separately.
"""

import argparse
import struct
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional

import serial

from packet import (
    BASE_FRAME_DATA_LEN,
    FRAME_M0,
    FRAME_M1,
    LORA_PAYLOAD_SIZE,
    crc8,
    decode_full_state,
)

_ST_WAIT_M0 = 0
_ST_WAIT_M1 = 1
_ST_WAIT_LEN = 2
_ST_READ_DATA = 3
_ST_CHECK_CRC = 4


@dataclass
class NodeStats:
    node_id: int
    first_seq: Optional[int] = None
    last_seq: Optional[int] = None
    received: int = 0
    missing: int = 0
    duplicates: int = 0
    out_of_order: int = 0
    crc_valid_packets: int = 0
    last_rssi: Optional[int] = None
    seq_history: List[int] = field(default_factory=list)

    def observe(self, seq: int, rssi: int) -> None:
        seq &= 0xFF
        self.last_rssi = rssi
        self.crc_valid_packets += 1

        if self.first_seq is None:
            self.first_seq = seq
            self.last_seq = seq
            self.received = 1
            self.seq_history.append(seq)
            return

        assert self.last_seq is not None

        if seq == self.last_seq:
            self.duplicates += 1
            return

        forward_gap = (seq - self.last_seq) & 0xFF
        backward_gap = (self.last_seq - seq) & 0xFF

        # Normal forward progression.
        if 1 <= forward_gap <= 127:
            if forward_gap > 1:
                self.missing += forward_gap - 1
            self.last_seq = seq
            self.received += 1
            self.seq_history.append(seq)
            return

        # Anything else is treated as out-of-order / stale.
        if backward_gap >= 1:
            self.out_of_order += 1
            return

    @property
    def expected(self) -> int:
        return self.received + self.missing

    @property
    def loss_percent(self) -> float:
        if self.expected <= 0:
            return 0.0
        return 100.0 * self.missing / float(self.expected)


class FrameReceiver:
    def __init__(self) -> None:
        self.state = _ST_WAIT_M0
        self.buf = bytearray()
        self.expected_len = 0
        self.crc_failures = 0
        self.length_failures = 0

    def push_byte(self, b: int) -> Optional[dict]:
        if self.state == _ST_WAIT_M0:
            if b == FRAME_M0:
                self.state = _ST_WAIT_M1
            return None

        if self.state == _ST_WAIT_M1:
            self.state = _ST_WAIT_LEN if b == FRAME_M1 else _ST_WAIT_M0
            return None

        if self.state == _ST_WAIT_LEN:
            self.expected_len = b
            if self.expected_len != BASE_FRAME_DATA_LEN:
                self.length_failures += 1
                self.state = _ST_WAIT_M0
                return None
            self.buf = bytearray([b])
            self.state = _ST_READ_DATA
            return None

        if self.state == _ST_READ_DATA:
            self.buf.append(b)
            if len(self.buf) == 1 + self.expected_len:
                self.state = _ST_CHECK_CRC
            return None

        if self.state == _ST_CHECK_CRC:
            received_crc = b
            computed_crc = crc8(bytes(self.buf))

            self.state = _ST_WAIT_M0

            if received_crc != computed_crc:
                self.crc_failures += 1
                self.buf = bytearray()
                return None

            rssi = struct.unpack_from("<b", self.buf, 1)[0]
            raw_payload = bytes(self.buf[2 : 2 + LORA_PAYLOAD_SIZE])
            self.buf = bytearray()

            pkt = decode_full_state(raw_payload, rssi)
            return pkt

        self.state = _ST_WAIT_M0
        self.buf = bytearray()
        return None


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="SmartFires packet loss integration test")
    p.add_argument("--port", default="/dev/ttyTHS1", help="Serial port")
    p.add_argument("--baud", type=int, default=115200, help="Baud rate")
    p.add_argument("--duration", type=int, default=60, help="Test duration in seconds")
    p.add_argument(
        "--nodes",
        type=int,
        nargs="+",
        default=[1, 2],
        help="Node IDs to track, e.g. --nodes 1 2",
    )
    p.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress per-packet prints; show summary only",
    )
    return p.parse_args()


def run_test(
    port: str, baud: int, duration_s: int, nodes: List[int], quiet: bool
) -> int:
    tracked_nodes = {node_id: NodeStats(node_id=node_id) for node_id in nodes}
    receiver = FrameReceiver()

    start = time.monotonic()
    deadline = start + duration_s
    total_decoded = 0
    untracked_packets = 0

    print(f"SmartFires packet loss test")
    print(f"Port: {port}  Baud: {baud}  Duration: {duration_s}s  Nodes: {nodes}")
    print("Listening...\n")

    try:
        with serial.Serial(port, baud, timeout=0.25) as ser:
            while time.monotonic() < deadline:
                raw = ser.read(1)
                if not raw:
                    continue

                pkt = receiver.push_byte(raw[0])
                if pkt is None:
                    continue

                total_decoded += 1

                node_id = int(pkt["node_id"])
                seq = int(pkt["seq"]) & 0xFF
                rssi = int(pkt["rssi"])

                if node_id not in tracked_nodes:
                    untracked_packets += 1
                    if not quiet:
                        print(f"[IGN] node={node_id} seq={seq:3d} rssi={rssi:4d}")
                    continue

                tracked_nodes[node_id].observe(seq=seq, rssi=rssi)

                if not quiet:
                    stats = tracked_nodes[node_id]
                    print(
                        f"[RX] node={node_id} seq={seq:3d} rssi={rssi:4d} "
                        f"recv={stats.received:4d} miss={stats.missing:4d} "
                        f"dup={stats.duplicates:3d} ooo={stats.out_of_order:3d}"
                    )

    except serial.SerialException as exc:
        print(f"Serial error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\nInterrupted by user.\n")

    print("\n=== Summary ===")
    print(f"Decoded packets:      {total_decoded}")
    print(f"CRC failures:         {receiver.crc_failures}")
    print(f"Length mismatches:    {receiver.length_failures}")
    print(f"Untracked packets:    {untracked_packets}")
    print()

    exit_code = 0

    for node_id in nodes:
        s = tracked_nodes[node_id]
        print(f"Node {node_id}")
        print(f"  First seq:          {s.first_seq}")
        print(f"  Last seq:           {s.last_seq}")
        print(f"  Received:           {s.received}")
        print(f"  Missing:            {s.missing}")
        print(f"  Expected:           {s.expected}")
        print(f"  Loss %:             {s.loss_percent:.2f}")
        print(f"  Duplicates:         {s.duplicates}")
        print(f"  Out-of-order:       {s.out_of_order}")
        print(f"  Last RSSI:          {s.last_rssi}")
        print()

        if s.received == 0:
            print(f"[FAIL] Node {node_id}: no packets received")
            exit_code = 1
        elif s.loss_percent > 5.0:
            print(f"[FAIL] Node {node_id}: packet loss exceeds 5%")
            exit_code = 1
        else:
            print(f"[PASS] Node {node_id}: packet loss within threshold")
        print()

    return exit_code


if __name__ == "__main__":
    args = parse_args()
    sys.exit(run_test(args.port, args.baud, args.duration, args.nodes, args.quiet))
