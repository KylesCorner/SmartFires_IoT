import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from smartfires_edge.csv_logger import DurableCsvLogger
from smartfires_edge.packet_loss import PacketLossTracker
from smartfires_edge.state_store import atomic_write_json
from smartfires_edge.uart_receiver import iter_packets


def _append_jsonl(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(payload, sort_keys=True) + "\n")
        f.flush()
        os.fsync(f.fileno())


def run_receive(
    port: str,
    baud: int,
    data_dir: Path,
    nodes: list[int],
    metrics_interval_s: int,
    fsync_every_row: bool,
    raw_log: bool,
) -> int:
    telemetry_dir = data_dir / "telemetry"
    metrics_dir = data_dir / "metrics"
    raw_dir = data_dir / "raw"

    logger = DurableCsvLogger(telemetry_dir, fsync_every_row=fsync_every_row)
    tracker = PacketLossTracker(nodes)

    state_path = metrics_dir / "packet_loss_state.json"
    last_metrics_write = 0.0

    print(f"SmartFires edge receive")
    print(f"Port: {port}  Baud: {baud}")
    print(f"Data dir: {data_dir}")
    print(f"Tracked nodes: {nodes}")
    print()

    try:
        for pkt, receiver in iter_packets(port, baud):
            tracker.crc_failures = receiver.crc_failures
            tracker.length_failures = receiver.length_failures

            logger.write_row(pkt)

            tracker.observe_packet(
                node_id=int(pkt["node_id"]),
                seq=int(pkt["seq"]),
                rssi=int(pkt["rssi"]),
            )

            if raw_log:
                raw_path = (
                    raw_dir
                    / f"frames-{datetime.now(timezone.utc).strftime('%Y-%m-%d')}.jsonl"
                )
                _append_jsonl(raw_path, pkt)

            print(
                f"[RX] node={pkt['node_id']} seq={pkt['seq']:3d} "
                f"T={pkt['temp_c']:5.1f}C H={pkt['humidity_pct']:4.1f}% "
                f"wind={pkt['wind_mps']:.2f} PM2.5={pkt['pm2_5_ug_m3']:.1f} "
                f"rssi={pkt['rssi']:4d}"
            )

            now = time.monotonic()
            if now - last_metrics_write >= metrics_interval_s:
                tracker.save(state_path)
                last_metrics_write = now

    except KeyboardInterrupt:
        print("\nStopped by user.")
    except Exception as exc:
        print(f"\n[FATAL] {exc}", file=sys.stderr)
        tracker.save(state_path)
        logger.close()
        return 1

    tracker.save(state_path)
    logger.close()
    return 0
