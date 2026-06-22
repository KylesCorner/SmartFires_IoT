"""Passive LoRa sniffer ingest — reads NDJSON from the sniffer Feather over USB
serial, decodes SmartFires packets, and aligns every reception against the
TDMA schedule the base station is actually broadcasting.

The sniffer firmware (``platformio/src/main_lora_sniffer.cpp``) does no
SmartFires-aware decoding itself; this module owns all packet naming, TDMA
slot math, and live-state updates, mirroring the role ``ingest_service.py``
plays for the base-station UART link.
"""

import json
import struct
import sys
from datetime import datetime, timezone
from typing import Callable, Optional

import serial

from smartfires_edge.config import SnifferConfig
from smartfires_edge.live_state import LiveState
from smartfires_edge.packet import (
    HEADER_FMT,
    PKT_AWAKEN,
    PKT_BUNDLE,
    PKT_FULL_STATE,
    PKT_GPS,
    PKT_MAGIC,
    PKT_STATUS,
    PKT_TIME_SYNC,
    decode_awaken,
    decode_bundle,
    decode_full_state,
    decode_status,
    decode_time_sync,
)

# TDMA geometry — must match platformio/include/radio/TdmaConfig.h defaults.
# Not CLI-tunable: changing slot width/guard requires a firmware rebuild on
# every node, at which point this constant should be updated too.
SLOT_WIDTH_MS = 900
GUARD_MS = 20


def _pkt_type_name(pkt_type: int) -> str:
    return {
        PKT_AWAKEN: "AWAKEN",
        PKT_FULL_STATE: "FULL_STATE",
        PKT_BUNDLE: "BUNDLE",
        PKT_STATUS: "STATUS",
        PKT_TIME_SYNC: "TIME_SYNC",
    }.get(pkt_type, f"0x{pkt_type:02x}")


class _SyncAnchor:
    """Tracks the most recently overheard TIME_SYNC broadcast.

    Re-anchors on every TIME_SYNC heard (not just the first) so slot timing
    always reflects the live session clock the base station is currently
    broadcasting, and naturally re-anchors to a new session if the base
    station restarts (session_id changes).
    """

    def __init__(self) -> None:
        self.sniffer_t_ms: Optional[int] = None
        self.session_time_ms: Optional[int] = None
        self.session_id: Optional[int] = None

    @property
    def anchored(self) -> bool:
        return self.sniffer_t_ms is not None

    def apply(self, t_ms: int, session_id: int, session_time_ms: int) -> bool:
        """Update the anchor. Returns True if this is a new session (caller
        should reset per-node sniffer stats)."""
        session_changed = self.session_id is not None and self.session_id != session_id
        self.sniffer_t_ms = t_ms
        self.session_time_ms = session_time_ms
        self.session_id = session_id
        return session_changed

    def session_ms_for(self, t_ms: int) -> Optional[int]:
        if not self.anchored:
            return None
        return self.session_time_ms + (t_ms - self.sniffer_t_ms)


def _slot_timing(node_id: int, session_ms: int, num_slots: int) -> tuple[float, bool]:
    """Return (jitter_ms, guard_violation) for a packet from node_id arriving
    at session_ms, given the compile-time slot assignment slot=(node_id-1)%num_slots."""
    frame_period_ms = num_slots * SLOT_WIDTH_MS
    slot = (node_id - 1) % num_slots
    slot_center_ms = slot * SLOT_WIDTH_MS + SLOT_WIDTH_MS / 2
    frame_phase_ms = session_ms % frame_period_ms
    jitter_ms = frame_phase_ms - slot_center_ms
    guard_violation = abs(jitter_ms) > (SLOT_WIDTH_MS / 2 - GUARD_MS)
    return jitter_ms, guard_violation


def _peek_header(raw: bytes) -> tuple[Optional[int], Optional[int], Optional[int]]:
    """Best-effort header peek (pkt_type, node_id, seq) without CRC validation,
    used only for logging when full decode fails."""
    if len(raw) < 4:
        return None, None, None
    magic, pkt_type, node_id, seq = struct.unpack_from(HEADER_FMT, raw, 0)
    if magic != PKT_MAGIC:
        return None, None, None
    return pkt_type, node_id, seq


def _decode_rx_event(
    rx: dict,
    anchor: _SyncAnchor,
    num_slots: int,
) -> Optional[dict]:
    payload_hex = rx.get("payload_hex", "")
    try:
        raw = bytes.fromhex(payload_hex)
    except ValueError:
        return None

    pkt_type, hdr_node_id, hdr_seq = _peek_header(raw)
    rssi = rx.get("rssi")
    snr = rx.get("snr")
    t_ms = int(rx.get("t_ms", 0))

    node_id = hdr_node_id
    seq = hdr_seq
    session_changed = False
    extra: dict = {}

    if pkt_type == PKT_TIME_SYNC:
        ts = decode_time_sync(raw, rssi)
        if ts is not None:
            session_changed = anchor.apply(t_ms, ts["session_id"], ts["session_time_ms"])
            extra["session_id"] = ts["session_id"]
    elif pkt_type == PKT_AWAKEN:
        dec = decode_awaken(raw, rssi)
        if dec is not None:
            extra["uid_hash"] = dec["uid_hash"]
    elif pkt_type in (PKT_STATUS, PKT_GPS):
        dec = decode_status(raw, rssi)
        if dec is not None:
            extra["battery_pct"] = dec.get("battery_pct")
    elif pkt_type == PKT_FULL_STATE:
        decode_full_state(raw, rssi)
    elif pkt_type == PKT_BUNDLE:
        decode_bundle(raw, rssi)

    session_ms = anchor.session_ms_for(t_ms) if pkt_type != PKT_TIME_SYNC else anchor.session_time_ms
    jitter_ms = None
    guard_violation = None
    if session_ms is not None and node_id is not None and pkt_type != PKT_TIME_SYNC:
        jitter_ms, guard_violation = _slot_timing(node_id, session_ms, num_slots)

    event = {
        "wall_t": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
        "sniffer_t_ms": t_ms,
        "session_ms": session_ms,
        "pkt_type": _pkt_type_name(pkt_type) if pkt_type is not None else "UNKNOWN",
        "node_id": node_id,
        "seq": seq,
        "rssi": rssi,
        "snr": snr,
        "jitter_ms": round(jitter_ms, 1) if jitter_ms is not None else None,
        "guard_violation": guard_violation,
        "anchored": anchor.anchored,
        "payload_hex": payload_hex,
    }
    event.update(extra)
    event["_session_changed"] = session_changed
    return event


def run_sniffer(
    cfg: SnifferConfig,
    live_state: LiveState,
    log_fn: Callable[[str, Optional[int]], None] | None = None,
) -> int:
    """Run the sniffer ingest loop. Intended to be run in a daemon thread
    alongside the base-station ingest loop (see web_service.run_web)."""
    if log_fn is None:
        log_fn = lambda msg, node_id=None: print(msg)  # noqa: E731

    anchor = _SyncAnchor()
    log_fn(f"[SNIFFER] Listening on {cfg.port} @ {cfg.baud}, num_slots={cfg.num_slots}", None)

    try:
        with serial.Serial(cfg.port, cfg.baud, timeout=1.0) as ser:
            while True:
                line = ser.readline()
                if not line:
                    continue
                try:
                    msg = json.loads(line.decode("utf-8", errors="replace").strip())
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue

                if msg.get("event") != "rx":
                    if msg.get("event") in ("status", "error", "config"):
                        log_fn(f"[SNIFFER] {msg.get('message') or msg}", None)
                    continue

                event = _decode_rx_event(msg, anchor, cfg.num_slots)
                if event is None:
                    continue

                if event.pop("_session_changed", False):
                    live_state.reset_sniffer_stats()
                    log_fn("[SNIFFER] New base station session detected — sniffer stats reset", None)

                live_state.push_sniffer_event(event)
                log_fn(
                    f"[SNIFFER-RX] type={event['pkt_type']} node={event['node_id']} "
                    f"rssi={event['rssi']} snr={event['snr']} jitter={event['jitter_ms']}",
                    event["node_id"],
                )
    except serial.SerialException as exc:
        print(f"[SNIFFER][FATAL] {exc}", file=sys.stderr)
        log_fn(f"[SNIFFER] serial error: {exc}", None)
        return 1
    except KeyboardInterrupt:
        log_fn("[SNIFFER] Stopped by user.", None)
    return 0
