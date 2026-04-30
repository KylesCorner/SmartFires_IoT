import json
import os
import random
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

import serial

from smartfires_edge.anemometer import AnemometerPoller
from smartfires_edge.csv_logger import DurableCsvLogger
from smartfires_edge.packet import (
    PKT_BUNDLE,
    PKT_FULL_STATE,
    PKT_STATUS,
    encode_ack_summary_frame,
    encode_time_sync_frame,
)
from smartfires_edge.packet_loss import PacketLossTracker
from smartfires_edge.uart_receiver import iter_packets


def _append_jsonl(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(payload, sort_keys=True) + "\n")
        f.flush()
        os.fsync(f.fileno())


def _seq_ahead(base: int, seq: int) -> int:
    return (seq - base) & 0xFF


def _ack_state_update(state: dict, seq: int) -> None:
    if not state.get("init", False):
        state["base"] = (seq - 1) & 0xFF
        state["init"] = True

    received = state.setdefault("received", set())
    received.add(seq & 0xFF)

    base = state["base"]
    while True:
        nxt = (base + 1) & 0xFF
        if nxt not in received:
            break
        received.remove(nxt)
        base = nxt
    state["base"] = base

    to_drop = []
    for s in received:
        d = _seq_ahead(base, s)
        if d == 0 or d > 16:
            to_drop.append(s)
    for s in to_drop:
        received.discard(s)


def _ack_state_mask(state: dict) -> int:
    if not state.get("init", False):
        return 0
    base = state["base"]
    mask = 0
    for s in state.get("received", set()):
        d = _seq_ahead(base, s)
        if 1 <= d <= 16:
            mask |= 1 << (d - 1)
    return mask


def _time_sync_sender(
    ser: serial.Serial,
    write_lock: threading.Lock,
    session_id: int,
    session_start: float,
    interval_s: int,
) -> None:
    seq = 1
    while True:
        time.sleep(interval_s)
        session_ms = int((time.time() - session_start) * 1000) & 0xFFFFFFFF
        frame = encode_time_sync_frame(session_id, session_ms, seq)
        with write_lock:
            try:
                ser.write(frame)
            except serial.SerialException as exc:
                print(f"[TIME_SYNC] write error: {exc}", file=sys.stderr)
        seq = (seq + 1) & 0xFF


def run_receive(
    port: str,
    baud: int,
    data_dir: Path,
    nodes: list[int],
    metrics_interval_s: int,
    sync_interval_s: int,
    ack_interval_s: float,
    fsync_every_row: bool,
    raw_log: bool,
    anemometer_port: str | None,
    anemometer_baud: int,
    anemometer_address: int,
    anemometer_interval_s: float,
) -> int:
    telemetry_dir = data_dir / "telemetry"
    metrics_dir = data_dir / "metrics"
    raw_dir = data_dir / "raw"

    logger = DurableCsvLogger(telemetry_dir, fsync_every_row=fsync_every_row)
    tracker = PacketLossTracker(nodes)
    node_gps: dict[int, tuple[float, float]] = {}
    ack_state: dict[int, dict] = {}
    ack_seq = 0
    next_ack_at = time.time() + ack_interval_s

    session_id = random.randint(1, 0xFFFFFFFF)
    session_start = time.time()

    anemometer: AnemometerPoller | None = None
    if anemometer_port:
        anemometer = AnemometerPoller(
            port=anemometer_port,
            baud=anemometer_baud,
            address=anemometer_address,
            interval_s=anemometer_interval_s,
        )
        anemometer.start()

    state_path = metrics_dir / "packet_loss_state.json"
    last_metrics_write = 0.0

    print(f"SmartFires edge receive")
    print(f"Port: {port}  Baud: {baud}")
    print(f"Data dir: {data_dir}")
    print(f"Tracked nodes: {nodes}")
    print(f"Session ID: 0x{session_id:08x}  TIME_SYNC interval: {sync_interval_s}s")
    if anemometer_port:
        print(
            "Anemometer: "
            f"{anemometer_port} @ {anemometer_baud} addr={anemometer_address} "
            f"interval={anemometer_interval_s}s"
        )
    print()

    try:
        sync_thread_started = False
        write_lock = threading.Lock()

        for event, receiver, ser in iter_packets(port, baud):
            if not sync_thread_started:
                with write_lock:
                    ser.write(encode_time_sync_frame(session_id, 0, seq=0))
                sync_thread = threading.Thread(
                    target=_time_sync_sender,
                    args=(ser, write_lock, session_id, session_start, sync_interval_s),
                    daemon=True,
                )
                sync_thread.start()
                sync_thread_started = True

            tracker.crc_failures = receiver.crc_failures
            tracker.length_failures = receiver.length_failures

            gps = event.get("gps")
            if gps:
                node_gps[int(gps["node_id"])] = (float(gps["lat"]), float(gps["lon"]))

            for pkt in event.get("packets", []):
                gps_fix = node_gps.get(int(pkt["node_id"]))
                pkt["lat"] = gps_fix[0] if gps_fix else ""
                pkt["lon"] = gps_fix[1] if gps_fix else ""

                if anemometer is not None:
                    jetson_speed, jetson_dir = anemometer.latest()
                    pkt["jetson_wind_mps"] = jetson_speed if jetson_speed is not None else ""
                    pkt["jetson_wind_dir_deg"] = jetson_dir if jetson_dir is not None else ""
                else:
                    pkt["jetson_wind_mps"] = ""
                    pkt["jetson_wind_dir_deg"] = ""

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

            hdr_node = event.get("node_id")
            hdr_seq = event.get("seq")
            pkt_type = event.get("pkt_type")
            if (
                hdr_node is not None
                and hdr_seq is not None
                and pkt_type in (PKT_FULL_STATE, PKT_BUNDLE, PKT_STATUS)
            ):
                st = ack_state.setdefault(int(hdr_node), {"init": False, "base": 0, "received": set()})
                _ack_state_update(st, int(hdr_seq))

            now = time.time()
            if now >= next_ack_at and ack_state:
                for node_id, st in ack_state.items():
                    if not st.get("init", False):
                        continue
                    frame = encode_ack_summary_frame(
                        node_id=node_id,
                        ack_base_seq=st["base"],
                        ack_mask=_ack_state_mask(st),
                        seq=ack_seq,
                    )
                    with write_lock:
                        try:
                            ser.write(frame)
                        except serial.SerialException as exc:
                            print(f"[ACK_SUM] write error: {exc}", file=sys.stderr)
                    ack_seq = (ack_seq + 1) & 0xFF
                next_ack_at = now + ack_interval_s

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
        if anemometer is not None:
            anemometer.stop()
        return 1

    tracker.save(state_path)
    logger.close()
    if anemometer is not None:
        anemometer.stop()
    return 0
