import json
import queue
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
from smartfires_edge.debug_log import parse_sfdbg_line
from smartfires_edge.live_state import LiveState
from smartfires_edge.packet import (
    PKT_AWAKEN,
    PKT_BUNDLE,
    PKT_CMD_ACK,
    PKT_DEBUG_LOG,
    PKT_FULL_STATE,
    PKT_STATUS,
    encode_cmd_reset_frame,
    encode_time_sync_frame,
)
from smartfires_edge.packet_loss import PacketLossTracker
from smartfires_edge.session import SessionManager
from smartfires_edge.session_meta import SessionMetaLogger
from smartfires_edge.uart_receiver import iter_packets


def _append_jsonl(path: Path, payload: dict) -> None:
    # flush() (not fsync()) deliberately: this runs inline in the same loop
    # that drains the base station's serial port one byte at a time
    # (uart_receiver.iter_packets). fsync() blocks until the write physically
    # lands on disk; on a slow/busy disk that stall can outlast the OS's
    # serial input buffer, dropping bytes and desyncing FrameReceiver's frame
    # parser for the rest of the session. flush() just hands the bytes to the
    # OS and returns immediately, matching DurableCsvLogger's default
    # (fsync_every_row=False) for the same reason.
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(payload, sort_keys=True) + "\n")
        f.flush()


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


def _send_cmd_reset(
    ser: serial.Serial,
    write_lock: threading.Lock,
    cmd_seq_state: dict,
    node_id: int,
    reset_type: int,
    log_fn: Callable[[str, int | None], None],
) -> bool:
    """Send a CMD_RESET frame. node_id=0 means "reset the base station itself"."""
    with write_lock:
        seq = int(cmd_seq_state.setdefault("next_seq", 0)) & 0xFF
        frame = encode_cmd_reset_frame(node_id=node_id, reset_type=reset_type, seq=seq)
        try:
            ser.write(frame)
        except serial.SerialException as exc:
            print(f"[EDGE][CMD-RESET] write error: {exc}", file=sys.stderr)
            return False
        cmd_seq_state["next_seq"] = (seq + 1) & 0xFF
    kind = "hard" if reset_type == 0x01 else "soft"
    log_fn(f"[EDGE][CMD-RESET] node={node_id} seq={seq:03d} reset_type={kind} bytes={len(frame)}", node_id or None)
    return True


def _time_sync_sender(
    ser: serial.Serial,
    write_lock: threading.Lock,
    sync_state: dict,
    session_ctx: dict,
    interval_s: int,
    log_fn: Callable[[str, int | None], None],
    stop_event: threading.Event,
) -> None:
    # stop_event is per-connection: when the serial link drops and the ingest
    # loop reconnects, it gets a fresh `ser` and must stop this thread rather
    # than let it keep writing to the now-dead handle forever in the background.
    while not stop_event.wait(interval_s):
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
    node_reset_queue: "queue.Queue[int] | None" = None,
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
        node_reset_queue: Optional queue of node_ids, populated by the web API's
                           per-node "Reset" button. Drained once per loop tick —
                           each entry triggers a hard CMD_RESET to that node.
    """
    if log_fn is None:
        log_fn = lambda msg, node_id=None, source="ingest", kind="other": print(msg)  # noqa: E731

    tracker = PacketLossTracker(cfg.nodes)
    if live_state is not None:
        live_state.tracker = tracker
    sync_state = {"next_seq": 0}
    cmd_seq_state = {"next_seq": 0}
    session_manager = SessionManager()
    if live_state is not None:
        # Carry forward node serials (uid_hash) learned in prior runs — they're
        # persisted in session.json by SessionManager and tied to hardware, not
        # to this particular ingest session, so the dashboard shouldn't have to
        # wait for a fresh AWAKEN before showing them.
        for node_id in cfg.nodes:
            uid_hash = session_manager.get_uid_hash_for_node(node_id)
            if uid_hash is not None:
                live_state.set_uid_hash(node_id, uid_hash)

    session_id = random.randint(1, 0xFFFFFFFF)
    session_start = time.time()
    session_stamp = _make_session_stamp(session_start)
    session_ctx = {"session_id": session_id, "session_start": session_start}
    if live_state is not None:
        live_state.set_session(session_id, session_start)

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

    write_lock = threading.Lock()
    reconnect_delay_s = 1.0
    max_reconnect_delay_s = 10.0

    try:
        while True:
            sync_thread_started = False
            sync_stop_event = threading.Event()

            try:
                if live_state is not None:
                    live_state.set_link_connected(True)

                for event, receiver, ser in iter_packets(cfg.port, cfg.baud, session_start):
                    if not sync_thread_started:
                        _send_cmd_reset(ser, write_lock, cmd_seq_state, node_id=0, reset_type=0, log_fn=log_fn)
                        time.sleep(0.5)  # let the base's RH_RF95.begin() reinit complete
                        _send_time_sync(
                            ser=ser,
                            write_lock=write_lock,
                            sync_state=sync_state,
                            session_ctx=session_ctx,
                            reason="session_start",
                            log_fn=log_fn,
                        )

                        sync_thread = threading.Thread(
                            target=_time_sync_sender,
                            args=(
                                ser,
                                write_lock,
                                sync_state,
                                session_ctx,
                                cfg.sync_interval_s,
                                log_fn,
                                sync_stop_event,
                            ),
                            daemon=True,
                        )
                        sync_thread.start()
                        sync_thread_started = True
                        reconnect_delay_s = 1.0

                    tracker.crc_failures = receiver.crc_failures
                    tracker.length_failures = receiver.length_failures

                    hdr_node = event.get("node_id")
                    hdr_seq = event.get("seq")
                    pkt_type = event.get("pkt_type")

                    # Base-originated debug log line (FramedDebugLogSink), never a
                    # LoRa packet from a real node — handled entirely separately from
                    # telemetry/loss-tracking below, then skip the rest of the loop
                    # body for this iteration.
                    if pkt_type == PKT_DEBUG_LOG:
                        debug_text = event.get("debug_log")
                        if live_state is not None and debug_text is not None:
                            record = parse_sfdbg_line(debug_text) or {
                                "v": "?",
                                "node": "?",
                                "src": "?",
                                "lvl": "?",
                                "seq": "-",
                                "t": "-",
                                "msg": debug_text,
                                "raw": debug_text,
                            }
                            live_state.push_base_debug(record)
                        continue

                    log_fn(
                        f"[EDGE][LORA-RX] type={_pkt_type_name(pkt_type)} node={hdr_node} "
                        f"seq={hdr_seq} rssi={event.get('rssi')}",
                        int(hdr_node) if hdr_node is not None else None,
                    )

                    if hdr_node is not None and pkt_type == PKT_AWAKEN:
                        # Node rebooted, so its wire seq counter restarted from 0 —
                        # reset the loss-tracking baseline before observing this
                        # packet, or the gap since the old session's last seq gets
                        # miscounted as missing.
                        tracker.reset_node(int(hdr_node))

                    # Observe every packet (all types share the same rolling seq counter).
                    # Done once per LoRa packet here so that STATUS/AWAKEN seqs are counted
                    # and bundle samples don't inflate crc_valid_packets.
                    if hdr_node is not None and hdr_seq is not None and event.get("rssi") is not None:
                        tracker.observe_packet(
                            node_id=int(hdr_node),
                            seq=int(hdr_seq),
                            rssi=int(event["rssi"]),
                        )

                    if hdr_node is not None and hdr_seq is not None and pkt_type == PKT_AWAKEN:
                        awaken = event.get("awaken") or {}
                        uid_hash = awaken.get("uid_hash")
                        if uid_hash is not None:
                            aw = session_manager.on_awaken(int(hdr_node), int(uid_hash))
                            session_meta.on_awaken(int(hdr_node), int(uid_hash))
                            if live_state is not None:
                                live_state.set_uid_hash(int(hdr_node), int(uid_hash))
                            log_fn(
                                f"[EDGE][AWAKEN] node={aw['node_id']} uid=0x{aw['uid_hash']:08x}",
                                int(hdr_node),
                            )
                        # Log every AWAKEN as a CSV row so node reboots (e.g.
                        # watchdog-triggered restarts) are visible in telemetry.csv.
                        # A booting node re-broadcasts AWAKEN every 5 s until it
                        # receives TIME_SYNC, so one reboot may produce several rows.
                        awaken_row = {
                            "timestamp": datetime.utcnow().isoformat(timespec="milliseconds"),
                            "packet_type": "awaken",
                            "node_id": hdr_node,
                            "seq": hdr_seq,
                            "rssi": event.get("rssi"),
                            "uid_hash": f"0x{uid_hash:08x}" if isinstance(uid_hash, int) else "",
                        }
                        logger.write_row(awaken_row)
                        _append_jsonl(status_path, awaken_row)
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
                            f"lat={status_row['lat']} lon={status_row['lon']} "
                            f"gps_valid={status_row['gps_valid']} batt_valid={status_row['battery_valid']} "
                            f"batt_mv={status_row['battery_mv']} batt_pct={status_row['battery_pct']} "
                            f"rssi={status_row['rssi']} "
                            f"heading={status_row['heading_true_deg']} "
                            f"location_corrected_heading={status_row['location_corrected_heading']} "
                            f"retx_total={status_row['retx_total']} fail_total={status_row['fail_total']}",
                            int(status_row["node_id"]) if status_row["node_id"] is not None else None,
                            kind="status",
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
                        # GPS is logged only on status rows as it arrives; telemetry
                        # rows carry no position so the CSV never contains inferred
                        # data. Join telemetry to the latest status row downstream.
                        pkt["lat"] = ""
                        pkt["lon"] = ""

                        if anemometer is not None:
                            jetson_speed, jetson_dir = anemometer.latest()
                            pkt["jetson_wind_mps"] = jetson_speed if jetson_speed is not None else ""
                            pkt["jetson_wind_dir_deg"] = jetson_dir if jetson_dir is not None else ""
                        else:
                            pkt["jetson_wind_mps"] = ""
                            pkt["jetson_wind_dir_deg"] = ""

                        logger.write_row(pkt)
                        if live_state is not None:
                            live_state.record_telemetry(pkt)

                        if cfg.raw_log:
                            _append_jsonl(session_dir / "frames.jsonl", pkt)

                        log_fn(
                            f"[RX] node={pkt['node_id']} seq={pkt['seq']:3d} "
                            f"t={pkt['timestamp'][11:]} "
                            f"T={pkt['temp_c']:5.1f}C H={pkt['humidity_pct']:4.1f}% "
                            f"wind={pkt['wind_mps']:.2f} "
                            f"PM1.0={pkt['pm1_0_ug_m3']:.1f} PM2.5={pkt['pm2_5_ug_m3']:.1f} "
                            f"PM4.0={pkt['pm4_0_ug_m3']:.1f} PM10={pkt['pm10_ug_m3']:.1f} "
                            f"rssi={pkt['rssi']:4d}",
                            int(pkt["node_id"]),
                            kind="bundle",
                        )

                    now = time.monotonic()
                    if now - last_metrics_write >= cfg.metrics_interval_s:
                        tracker.save(state_path)
                        last_metrics_write = now

                    # Drain any per-node hard-reset requests queued by the web API's
                    # "Reset" button (one request per click; non-blocking).
                    if node_reset_queue is not None:
                        while True:
                            try:
                                target_node_id = node_reset_queue.get_nowait()
                            except queue.Empty:
                                break
                            _send_cmd_reset(
                                ser, write_lock, cmd_seq_state,
                                node_id=target_node_id, reset_type=0x01, log_fn=log_fn,
                            )

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
                            live_state.set_session(session_id, session_start)

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
                            kind="session",
                        )

            except (serial.SerialException, OSError) as exc:
                sync_stop_event.set()
                if live_state is not None:
                    live_state.set_link_connected(False, error=str(exc))
                log_fn(
                    f"[EDGE][LINK] base station serial link lost ({exc}) — "
                    f"retrying in {reconnect_delay_s:.0f}s",
                    None,
                    kind="error",
                )
                time.sleep(reconnect_delay_s)
                reconnect_delay_s = min(reconnect_delay_s * 2, max_reconnect_delay_s)

    except KeyboardInterrupt:
        log_fn("\nStopped by user.", None)
    except Exception as exc:
        print(f"\n[FATAL] {exc}", file=sys.stderr)
        if live_state is not None:
            live_state.set_link_connected(False, error=str(exc))
        tracker.save(state_path)
        logger.close()
        if anemometer is not None:
            anemometer.stop()
        return 1

    if live_state is not None:
        live_state.set_link_connected(False)
    tracker.save(state_path)
    logger.close()
    if anemometer is not None:
        anemometer.stop()
    return 0
