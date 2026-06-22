from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from smartfires_edge.state_store import atomic_write_json, read_json


@dataclass
class NodeStats:
    node_id: int
    first_seq: Optional[int] = None
    last_seq: Optional[int] = None
    seen_seqs: set = field(default_factory=set)
    crc_valid_packets: int = 0
    last_rssi: Optional[int] = None

    # Absolute (unwrapped) counterparts of first_seq/last_seq/seen_seqs, used
    # only for received/missing/duplicate accounting. The wire seq is 8-bit
    # and rolls over every 256 packets; without an unwrapped key, a seq reused
    # after a rollover collides with its own first-lap entry in seen_seqs and
    # gets miscounted as a duplicate forever after. first_seq/last_seq/seen_seqs
    # stay mod-256 because reception_timeline() needs the raw wire value.
    _first_abs_seq: Optional[int] = field(default=None, repr=False)
    _last_abs_seq: Optional[int] = field(default=None, repr=False)
    _seen_abs_seqs: set = field(default_factory=set, repr=False)

    def observe(self, seq: int, rssi: int) -> None:
        seq &= 0xFF
        self.last_rssi = rssi
        self.crc_valid_packets += 1

        if self.first_seq is None:
            self.first_seq = seq
            self.last_seq = seq
            self.seen_seqs.add(seq)
            self._first_abs_seq = seq
            self._last_abs_seq = seq
            self._seen_abs_seqs.add(seq)
            return

        self.seen_seqs.add(seq)

        # Advance last_seq when this packet is forward progress (within 127 ahead).
        # Packets behind last_seq are retransmissions or reorders — they are already
        # in seen_seqs and will reduce missing automatically at query time.
        forward_gap = (seq - self.last_seq) & 0xFF
        if 1 <= forward_gap <= 127:
            self.last_seq = seq

        # Resolve the wire seq onto an ever-increasing absolute sequence number
        # anchored to the previous absolute high-water mark, so rollovers
        # extend the count instead of wrapping back over earlier seqs.
        abs_delta = (seq - (self._last_abs_seq & 0xFF)) & 0xFF
        if abs_delta > 127:
            abs_delta -= 256
        abs_seq = self._last_abs_seq + abs_delta
        self._seen_abs_seqs.add(abs_seq)
        if abs_seq > self._last_abs_seq:
            self._last_abs_seq = abs_seq

    @property
    def _span(self) -> int:
        """Number of sequence positions from first_seq to last_seq inclusive."""
        if self._first_abs_seq is None:
            return 0
        return self._last_abs_seq - self._first_abs_seq + 1

    @property
    def received(self) -> int:
        """Unique sequence numbers received within the [first_seq, last_seq] window."""
        return len(self._seen_abs_seqs)

    @property
    def missing(self) -> int:
        return max(0, self._span - self.received)

    @property
    def duplicates(self) -> int:
        """Extra transmissions received beyond the first copy of each unique seq."""
        return max(0, self.crc_valid_packets - self.received)

    @property
    def expected(self) -> int:
        return self._span

    @property
    def loss_percent(self) -> float:
        if self._span <= 0:
            return 0.0
        return 100.0 * self.missing / self._span

    def to_dict(self) -> dict:
        return {
            "node_id": self.node_id,
            "first_seq": self.first_seq,
            "last_seq": self.last_seq,
            "received": self.received,
            "missing": self.missing,
            "duplicates": self.duplicates,
            "crc_valid_packets": self.crc_valid_packets,
            "last_rssi": self.last_rssi,
        }


class PacketLossTracker:
    def __init__(self, node_ids: list[int]) -> None:
        self.nodes = {node_id: NodeStats(node_id=node_id) for node_id in node_ids}
        self.crc_failures = 0
        self.length_failures = 0
        self.total_decoded = 0
        self.untracked_packets = 0

    def observe_packet(self, node_id: int, seq: int, rssi: int) -> None:
        self.total_decoded += 1
        if node_id not in self.nodes:
            self.nodes[node_id] = NodeStats(node_id=node_id)
        self.nodes[node_id].observe(seq=seq, rssi=rssi)

    def to_dict(self) -> dict:
        return {
            "updated_at": datetime.now(timezone.utc).isoformat(),
            "total_decoded": self.total_decoded,
            "crc_failures": self.crc_failures,
            "length_failures": self.length_failures,
            "untracked_packets": self.untracked_packets,
            "nodes": {str(k): v.to_dict() for k, v in self.nodes.items()},
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
        print(f"  First seq:     {stats['first_seq']}")
        print(f"  Last seq:      {stats['last_seq']}")
        print(f"  Received:      {stats['received']}")
        print(f"  Missing:       {stats['missing']}")
        print(f"  Duplicates:    {stats['duplicates']}")
        print(f"  Loss %:        {loss:.2f}")
        print(f"  Last RSSI:     {stats['last_rssi']}")
        print()
