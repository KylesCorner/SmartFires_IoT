import json
import os
import random
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

import serial

from smartfires_edge.anemometer import AnemometerPoller
from smartfires_edge.config import IngestConfig
from smartfires_edge.csv_logger import DurableCsvLogger
from smartfires_edge.live_state import LiveState
from smartfires_edge.packet import (
    PKT_AWAKEN,
    PKT_BUNDLE,
    PKT_CMD_ACK,
    PKT_FULL_STATE,
    PKT_STATUS,
    encode_time_sync_frame,
)
from smartfires_edge.packet_loss import PacketLossTracker
from smartfires_edge.session import SessionManager
from smartfires_edge.session_meta import SessionMetaLogger
from smartfires_edge.uart_receiver import iter_packets


def _append_jsonl(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(payload, sort_keys=True) + "\n")
        f.flush()
        os.fsync(f.fileno())


def _pkt_type_name(pkt_type: int | None) -> str:
    if pkt_type == PKT_AWAKEN:
        return "AWAKEN"
    if pkt_type == PKT_FULL_STATE:
        return "FULL_STATE"
    if pkt_type == PKT_BUNDLE:
        return "BUNDLE"
    if pkt_type == PKT_STATUS:
        return "STATUS"
    if pkt_type == PKT_CMD_ACK:
        return "CMD_ACK"
    return f"0x{(pkt_type or 0):02x}"


def _make_session_stamp(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d_%H%M%S")


def _send_time_sync(
    ser: serial.Serial,
    write_lock: threading.Lock,
    sync_state: dict,
    session_ctx: dict,
    reason: str,
    log_fn: Callable[[str, int | None], None],
    trigger_node: int | None = None,
    trigger_seq: int | None = None,
) -> bool:
    session_id = session_ctx["session_id"]
    session_start = session_ctx["session_start"]
    session_ms = int((time.time() - session_start) * 1000) & 0xFFFFFFFF
    with write_lock:
        seq = int(sync_state.setdefault("next_seq", 0)) & 0xFF
        frame = encode_time_sync_frame(session_id, session_ms, seq)
        try:
            ser.write(frame)
        except serial.SerialException as exc:
            print(f"[EDGE][SYNC] write error reason={reason}: {exc}", file=sys.stderr)
            return False
        sync_state["next_seq"] = (seq + 1) & 0xFF

    msg = (
        f"[EDGE][SYNC-TX#{seq:03d}] reason={reason} session_id=0x{session_id:08x} "
        f"session_ms={session_ms} bytes={len(frame)}"
    )
    if trigger_node is not None:
        msg += f" trigger_node={trigger_node}"
    if trigger_seq is not None:
        msg += f" trigger_seq={trigger_seq}"
    log_fn(msg, None)
    return True


def _time_sync_sender(
    ser: serial.Serial,
    write_lock: threading.Lock,
    sync_state: dict,
    session_ctx: dict,
    interval_s: int,
    log_fn: Callable[[str, int | None], None],
) -> None:
    while True:
        time.sleep(interval_s)
        _send_time_sync(
            ser=ser,
            write_lock=write_lock,
            sync_state=sync_state,
            session_ctx=session_ctx,
            reason="periodic",
            log_fn=log_fn,
        )


def run_receive(
    cfg: IngestConfig,
    live_state: LiveState | None = None,
    log_fn: Callable[[str, int | None], None] | None = None,
    reset_event: threading.Event | None = None,
) -> int:
    """Run the UART ingest loop.

    Args:
        cfg: All ingest settings sourced from :class:`~smartfires_edge.config.IngestConfig`
             (single source of truth — no more 10-parameter call sites).
        live_state: Optional shared state object injected by ``web`` subcommand
                    for live dashboard updates.
        log_fn: Optional log callback ``(msg, node_id)`` injected by ``web`` subcommand
                to stream log lines to the browser. Defaults to plain ``print``.
        reset_event: Optional threading.Event set by the web API to trigger a new session.
    """
    if log_fn is None:
        log_fn = lambda msg, node_id=None: print(msg)  # noqa: E731

    tracker = PacketLossTracker(cfg.nodes)
    if live_state is not None:
        live_state.tracker = tracker
    node_gps: dict[int, tuple[float, float]] = {}
    sync_state = {"next_seq": 0}
    session_manager = SessionManager()

    session_id = random.randint(1, 0xFFFFFFFF)
    session_start = time.time()
    session_stamp = _make_session_stamp(session_start)
    session_ctx = {"session_id": session_id, "session_start": session_start}

    session_dir = cfg.data_dir / session_stamp
    state_path = session_dir / "packet_loss_state.json"
    status_path = session_dir / "status.jsonl"

    logger = DurableCsvLogger(session_dir, fsync_every_row=cfg.fsync_every_row)

    session_meta = SessionMetaLogger(
        session_id=session_id,
        session_start=session_start,
        port=cfg.port,
        baud=cfg.baud,
        data_dir=session_dir,
    )

    anemometer: AnemometerPoller | None = None
    if cfg.anemometer.enabled:
        anemometer = AnemometerPoller(
            port=cfg.anemometer.port,
            baud=cfg.anemometer.baud,
            address=cfg.anemometer.address,
            interval_s=cfg.anemometer.interval_s,
        )
        anemometer.start()

    last_metrics_write = 0.0

    log_fn(f"SmartFires edge receive", None)
    log_fn(f"Port: {cfg.port}  Baud: {cfg.baud}", None)
    log_fn(f"Data dir: {cfg.data_dir}", None)
    log_fn(f"Tracked nodes: {cfg.nodes}", None)
    log_fn(f"Session ID: 0x{session_id:08x}  Stamp: {session_stamp}  TIME_SYNC interval: {cfg.sync_interval_s}s", None)
    if cfg.anemometer.enabled:
        log_fn(
            "Anemometer: "
            f"{cfg.anemometer.port} @ {cfg.anemometer.baud} addr={cfg.anemometer.address} "
            f"interval={cfg.anemometer.interval_s}s",
            None,
        )
    log_fn("", None)

    try:
        sync_thread_started = False
        write_lock = threading.Lock()

        for event, receiver, ser in iter_packets(cfg.port, cfg.baud, session_start):
            if not sync_thread_started:
                sync_thread = threading.Thread(
                    target=_time_sync_sender,
                    args=(
                        ser,
                        write_lock,
                        sync_state,
                        session_ctx,
                        cfg.sync_interval_s,
                        log_fn,
                    ),
                    daemon=True,
                )
                sync_thread.start()
                sync_thread_started = True

            tracker.crc_failures = receiver.crc_failures
            tracker.length_failures = receiver.length_failures

            hdr_node = event.get("node_id")
            hdr_seq = event.get("seq")
            pkt_type = event.get("pkt_type")

            log_fn(
                f"[EDGE][LORA-RX] type={_pkt_type_name(pkt_type)} node={hdr_node} "
                f"seq={hdr_seq} rssi={event.get('rssi')}",
                int(hdr_node) if hdr_node is not None else None,
            )

            if hdr_node is not None and hdr_seq is not None and pkt_type == PKT_AWAKEN:
                awaken = event.get("awaken") or {}
                uid_hash = awaken.get("uid_hash")
                if uid_hash is not None:
                    aw = session_manager.on_awaken(int(hdr_node), int(uid_hash))
                    session_meta.on_awaken(int(hdr_node), int(uid_hash))
                    log_fn(
                        f"[EDGE][AWAKEN] node={aw['node_id']} uid=0x{aw['uid_hash']:08x}",
                        int(hdr_node),
                    )
                log_fn(
                    f"[EDGE][AWAKEN] node={hdr_node} seq={hdr_seq} "
                    f"action=send_time_sync",
                    int(hdr_node),
                )
                _send_time_sync(
                    ser=ser,
                    write_lock=write_lock,
                    sync_state=sync_state,
                    session_ctx=session_ctx,
                    reason="awaken",
                    log_fn=log_fn,
                    trigger_node=int(hdr_node),
                    trigger_seq=int(hdr_seq),
                )
            elif hdr_node is not None and hdr_seq is not None and pkt_type is not None:
                _send_time_sync(
                    ser=ser,
                    write_lock=write_lock,
                    sync_state=sync_state,
                    session_ctx=session_ctx,
                    reason="receiver_start",
                    log_fn=log_fn,
                    trigger_node=int(hdr_node),
                    trigger_seq=int(hdr_seq),
                ) if sync_state["next_seq"] == 0 else None

            gps = event.get("gps")
            if gps:
                node_gps[int(gps["node_id"])] = (float(gps["lat"]), float(gps["lon"]))

            status = event.get("status")
            if status:
                uid_hash = session_manager.get_uid_hash_for_node(int(status.get("node_id")))
                heading = session_manager.on_status(
                    node_id=int(status.get("node_id")),
                    uid_hash=uid_hash,
                    status=status,
                )
                status_row = {
                    "timestamp": datetime.utcnow().isoformat(timespec="milliseconds"),
                    "packet_type": "status",
                    "node_id": status.get("node_id"),
                    "seq": status.get("seq"),
                    "session_time_ms": "",
                    "uptime_ms": "",
                    "sensor_flags": "",
                    "wind_mps": "",
                    "temp_c": "",
                    "humidity_pct": "",
                    "pm1_0_ug_m3": "",
                    "pm2_5_ug_m3": "",
                    "pm4_0_ug_m3": "",
                    "pm10_ug_m3": "",
                    "lat": status.get("lat"),
                    "lon": status.get("lon"),
                    "gps_valid": status.get("gps_valid"),
                    "battery_valid": status.get("battery_valid"),
                    "rssi": status.get("rssi"),
                    "flags": status.get("flags"),
                    "battery_mv": status.get("battery_mv"),
                    "battery_pct": status.get("battery_pct"),
                    "uid_hash": f"0x{uid_hash:08x}" if isinstance(uid_hash, int) else "",
                    "heading_true_deg": heading.get("heading_true_deg") if heading.get("computed") else "",
                    "location_corrected_heading": (
                        heading.get("location_corrected_heading")
                        if heading.get("computed") and heading.get("location_corrected_heading") is not None
                        else ""
                    ),
                    "jetson_wind_mps": "",
                    "jetson_wind_dir_deg": "",
                    "retx_total": status.get("retx_total") if status.get("retx_total") is not None else "",
                    "fail_total": status.get("fail_total") if status.get("fail_total") is not None else "",
                }
                _append_jsonl(status_path, status_row)
                logger.write_row(status_row)
                if live_state is not None:
                    live_state.record_status(status)
                log_fn(
                    f"[STATUS] node={status_row['node_id']} seq={status_row['seq']} "
                    f"gps_valid={status_row['gps_valid']} batt_valid={status_row['battery_valid']} "
                    f"batt_mv={status_row['battery_mv']} rssi={status_row['rssi']} "
                    f"heading={status_row['heading_true_deg']} "
                    f"location_corrected_heading={status_row['location_corrected_heading']} "
                    f"retx_total={status_row['retx_total']} fail_total={status_row['fail_total']}",
                    int(status_row["node_id"]) if status_row["node_id"] is not None else None,
                )

            cmd_ack = event.get("cmd_ack")
            if cmd_ack:
                session_manager.on_cmd_ack(
                    node_id=int(cmd_ack.get("node_id")),
                    uid_hash=int(cmd_ack.get("uid_hash")),
                    cmd_type=int(cmd_ack.get("cmd_type")),
                    status=int(cmd_ack.get("status")),
                )
                cmd_ack_row = {
                    "timestamp": datetime.utcnow().isoformat(timespec="milliseconds"),
                    "packet_type": "cmd_ack",
                    "node_id": cmd_ack.get("node_id"),
                    "seq": cmd_ack.get("seq"),
                    "cmd_type": cmd_ack.get("cmd_type"),
                    "uid_hash": cmd_ack.get("uid_hash"),
                    "status": cmd_ack.get("status"),
                    "rssi": cmd_ack.get("rssi"),
                }
                _append_jsonl(status_path, cmd_ack_row)
                log_fn(
                    "[CMD_ACK] "
                    f"node={cmd_ack_row['node_id']} seq={cmd_ack_row['seq']} "
                    f"cmd=0x{int(cmd_ack_row['cmd_type']):02x} "
                    f"uid=0x{int(cmd_ack_row['uid_hash']):08x} "
                    f"status={cmd_ack_row['status']} rssi={cmd_ack_row['rssi']}",
                    int(cmd_ack_row["node_id"]) if cmd_ack_row["node_id"] is not None else None,
                )

            for pkt in event.get("packets", []):
                pkt["packet_type"] = "telemetry"
                pkt["gps_valid"] = ""
                pkt["battery_valid"] = ""
                pkt["battery_mv"] = ""
                pkt["battery_pct"] = ""
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
                if live_state is not None:
                    live_state.record_telemetry(pkt)

                if cfg.raw_log:
                    _append_jsonl(session_dir / "frames.jsonl", pkt)

                log_fn(
                    f"[RX] node={pkt['node_id']} seq={pkt['seq']:3d} "
                    f"t={pkt['timestamp'][11:]} "
                    f"T={pkt['temp_c']:5.1f}C H={pkt['humidity_pct']:4.1f}% "
                    f"wind={pkt['wind_mps']:.2f} PM2.5={pkt['pm2_5_ug_m3']:.1f} "
                    f"rssi={pkt['rssi']:4d}",
                    int(pkt["node_id"]),
                )

            now = time.monotonic()
            if now - last_metrics_write >= cfg.metrics_interval_s:
                tracker.save(state_path)
                last_metrics_write = now

            # Check for new-session request from the web API
            if reset_event is not None and reset_event.is_set():
                reset_event.clear()
                tracker.save(state_path)
                logger.close()

                session_id = random.randint(1, 0xFFFFFFFF)
                session_start = time.time()
                session_stamp = _make_session_stamp(session_start)
                session_ctx["session_id"] = session_id
                session_ctx["session_start"] = session_start

                session_dir = cfg.data_dir / session_stamp
                state_path = session_dir / "packet_loss_state.json"
                status_path = session_dir / "status.jsonl"

                logger = DurableCsvLogger(session_dir, fsync_every_row=cfg.fsync_every_row)
                session_meta = SessionMetaLogger(
                    session_id=session_id,
                    session_start=session_start,
                    port=cfg.port,
                    baud=cfg.baud,
                    data_dir=session_dir,
                )

                tracker = PacketLossTracker(cfg.nodes)
                if live_state is not None:
                    live_state.tracker = tracker
                    live_state.reset()

                _send_time_sync(
                    ser=ser,
                    write_lock=write_lock,
                    sync_state=sync_state,
                    session_ctx=session_ctx,
                    reason="new_session",
                    log_fn=log_fn,
                )
                log_fn(
                    f"[SESSION] New session started: 0x{session_id:08x}  stamp={session_stamp}",
                    None,
                )

    except KeyboardInterrupt:
        log_fn("\nStopped by user.", None)
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
