from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from smartfires_edge.state_store import atomic_write_json, read_json


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

    def observe(self, seq: int, rssi: int) -> None:
        seq &= 0xFF
        self.last_rssi = rssi
        self.crc_valid_packets += 1

        if self.first_seq is None:
            self.first_seq = seq
            self.last_seq = seq
            self.received = 1
            return

        assert self.last_seq is not None

        if seq == self.last_seq:
            self.duplicates += 1
            return

        forward_gap = (seq - self.last_seq) & 0xFF
        backward_gap = (self.last_seq - seq) & 0xFF

        if 1 <= forward_gap <= 127:
            if forward_gap > 1:
                self.missing += forward_gap - 1
            self.last_seq = seq
            self.received += 1
            return

        if backward_gap >= 1:
            self.out_of_order += 1

    @property
    def expected(self) -> int:
        return self.received + self.missing

    @property
    def loss_percent(self) -> float:
        if self.expected <= 0:
            return 0.0
        return 100.0 * self.missing / self.expected


class PacketLossTracker:
    def __init__(self, node_ids: list[int]) -> None:
        self.nodes = {node_id: NodeStats(node_id=node_id) for node_id in node_ids}
        self.crc_failures = 0
        self.length_failures = 0
        self.total_decoded = 0
        self.untracked_packets = 0

    def observe_packet(self, node_id: int, seq: int, rssi: int) -> None:
        self.total_decoded += 1
        stats = self.nodes.get(node_id)
        if stats is None:
            self.untracked_packets += 1
            return
        stats.observe(seq=seq, rssi=rssi)

    def to_dict(self) -> dict:
        return {
            "updated_at": datetime.now(timezone.utc).isoformat(),
            "total_decoded": self.total_decoded,
            "crc_failures": self.crc_failures,
            "length_failures": self.length_failures,
            "untracked_packets": self.untracked_packets,
            "nodes": {str(k): asdict(v) for k, v in self.nodes.items()},
        }

    def save(self, path: Path) -> None:
        atomic_write_json(path, self.to_dict())


def print_summary(data_dir: Path) -> None:
    path = data_dir / "metrics" / "packet_loss_state.json"
    payload = read_json(path)

    if not payload:
        print("No packet-loss state found.")
        return

    print(f"Updated: {payload.get('updated_at')}")
    print(f"Total decoded: {payload.get('total_decoded', 0)}")
    print(f"CRC failures: {payload.get('crc_failures', 0)}")
    print(f"Length failures: {payload.get('length_failures', 0)}")
    print()

    for node_id, stats in payload.get("nodes", {}).items():
        expected = stats["received"] + stats["missing"]
        loss = 0.0 if expected == 0 else 100.0 * stats["missing"] / expected
        print(f"Node {node_id}")
        print(f"  Received:      {stats['received']}")
        print(f"  Missing:       {stats['missing']}")
        print(f"  Duplicates:    {stats['duplicates']}")
        print(f"  Out-of-order:  {stats['out_of_order']}")
        print(f"  Loss %:        {loss:.2f}")
        print(f"  Last RSSI:     {stats['last_rssi']}")
        print()
