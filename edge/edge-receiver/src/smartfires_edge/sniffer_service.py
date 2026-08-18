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
    HEADER_SIZE,
    PKT_FLAG_RETX,
    PKT_FLAG_WINDOW_FIRST,
    PKT_FLAG_WINDOW_LAST,
    PKT_ACK_SUMMARY,
    PKT_AWAKEN,
    PKT_BUNDLE,
    PKT_CMD_ACK,
    PKT_CMD_CALIBRATE,
    PKT_CMD_RESET,
    PKT_FULL_STATE,
    PKT_GPS,
    PKT_MAGIC,
    PKT_STATUS,
    PKT_TIME_SYNC,
    decode_ack_summary,
    decode_awaken,
    decode_bundle,
    decode_cmd_ack,
    decode_cmd_calibrate,
    decode_cmd_reset,
    decode_full_state,
    decode_status,
    decode_time_sync,
)

# TDMA geometry — must match platformio/include/radio/TdmaConfig.h defaults.
# Not CLI-tunable: changing slot width/guard requires a firmware rebuild on
# every node, at which point this constant should be updated too.
SLOT_WIDTH_MS = 900
GUARD_MS = 20

# The base station firmware now gates ACK_SUMMARY/CMD_CALIBRATE/CMD_RESET to
# its own reserved TDMA slot (slot 0, the identity that's never assigned to a
# real node — see config/BaseConfig.h's kFirstNodeId=2), so their jitter is
# computed against that virtual node_id=1 rather than skipped. TIME_SYNC
# stays excluded: it's the anchor itself, so its own jitter relative to
# itself is degenerate.
_VIRTUAL_BASE_NODE_ID = 1
_BASE_SLOTTED_TYPES = {PKT_ACK_SUMMARY, PKT_CMD_CALIBRATE, PKT_CMD_RESET}

# RadioHead link-layer header flags (RHReliableDatagram) — not part of the
# SmartFires wire protocol. See RadioHeadTdmaDriver.cpp's setHeaderFlags()
# calls for the firmware's matching usage.
RH_FLAGS_ACK = 0x80
RH_FLAGS_RETRY = 0x40

# RadioHead address of the base station (RadioHeadTdmaDriver::Config::radioHeadCfg(0x01)
# in main.cpp). Used to translate a bare RadioHead frame's attributed address
# (rh_owner_node_id) into the same node_id=0 convention used for
# SmartFires-decoded base packets, for slot-jitter math and detail display —
# it does not affect lane placement (see rh_owner_node_id docstring above).
_RH_BASE_ADDRESS = 1


def _pkt_type_name(pkt_type: int) -> str:
    return {
        PKT_AWAKEN: "AWAKEN",
        PKT_FULL_STATE: "FULL_STATE",
        PKT_BUNDLE: "BUNDLE",
        PKT_STATUS: "STATUS",
        PKT_TIME_SYNC: "TIME_SYNC",
        PKT_ACK_SUMMARY: "ACK_SUMMARY",
        PKT_CMD_CALIBRATE: "CMD_CALIBRATE",
        PKT_CMD_RESET: "CMD_RESET",
        PKT_CMD_ACK: "CMD_ACK",
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


def _peek_header(raw: bytes) -> tuple[Optional[int], Optional[int], Optional[int], int]:
    """Best-effort header peek (pkt_type, node_id, seq, flags) without CRC
    validation, used only for logging when full decode fails."""
    if len(raw) < HEADER_SIZE:
        return None, None, None, 0
    magic, pkt_type, node_id, seq, flags = struct.unpack_from(HEADER_FMT, raw, 0)
    if magic != PKT_MAGIC:
        return None, None, None, 0
    return pkt_type, node_id, seq, flags


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

    pkt_type, hdr_node_id, hdr_seq, hdr_flags = _peek_header(raw)
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
        # Timed duty-cycle window markers, so the sniffer view shows where each
        # active window starts and ends rather than an undifferentiated bundle
        # stream.
        if hdr_flags & PKT_FLAG_WINDOW_FIRST:
            extra["window_first"] = True
        if hdr_flags & PKT_FLAG_WINDOW_LAST:
            extra["window_last"] = True
        # A replay of a bundle the base never acked. It repeats the original's
        # window markers, so surface it alongside them — an apparent second
        # WINDOW_LAST inside one window is this, not a protocol fault.
        if hdr_flags & PKT_FLAG_RETX:
            extra["retx"] = True
    elif pkt_type == PKT_ACK_SUMMARY:
        # Header node_id stays 0 (broadcast convention) — these are base
        # station transmissions, so they land in the dedicated base lane.
        # The payload's own node_id is the node being acked; keep it as
        # metadata rather than reassigning the lane.
        dec = decode_ack_summary(raw, rssi)
        if dec is not None:
            extra["target_node_id"] = dec["node_id"]
            extra["ack_base_seq"] = dec["ack_base_seq"]
            extra["ack_mask"] = dec["ack_mask"]
    elif pkt_type == PKT_CMD_CALIBRATE:
        dec = decode_cmd_calibrate(raw)
        if dec is not None:
            extra["target_node_id"] = dec["node_id"]
    elif pkt_type == PKT_CMD_RESET:
        dec = decode_cmd_reset(raw)
        if dec is not None:
            extra["target_node_id"] = dec["node_id"]
    elif pkt_type == PKT_CMD_ACK:
        decode_cmd_ack(raw, rssi)

    rh_to = rx.get("rh_to")
    rh_from = rx.get("rh_from")
    rh_id = rx.get("rh_id")
    rh_flags = rx.get("rh_flags")
    rh_is_ack = bool(rh_flags is not None and rh_flags & RH_FLAGS_ACK)
    rh_is_retry = bool(rh_flags is not None and rh_flags & RH_FLAGS_RETRY)

    # rh_owner_node_id: best-effort attribution of which node's TDMA slot a
    # bare RadioHead frame (no SmartFires magic byte) belongs to. Kept
    # separate from `node_id` — these frames carry no SmartFires PktHeader,
    # so `node_id` stays None and the UI renders them on a dedicated
    # RadioHead/Unknown lane rather than mixing them into a node's lane.
    rh_owner_node_id = None
    if pkt_type is not None:
        pkt_type_name = _pkt_type_name(pkt_type)
    else:
        pkt_type_name = "RH_ACK" if rh_is_ack else "RH_RAW"
        if rh_is_ack:
            # An ACK is transmitted BY the receiver of the original message,
            # addressed back TO the original sender — so the TDMA slot this
            # exchange belongs to is owned by the ACK's *destination*
            # (rh_to), not the radio that's actually sending this ACK frame.
            attributed_addr = rh_to
        else:
            # Not a reliable-datagram ACK (e.g. a corrupted/foreign frame) —
            # no "who's being acknowledged" semantics, so attribute it to
            # whichever radio actually sent it.
            attributed_addr = rh_from
        if attributed_addr is not None:
            rh_owner_node_id = 0 if attributed_addr == _RH_BASE_ADDRESS else attributed_addr

    session_ms = anchor.session_ms_for(t_ms) if pkt_type != PKT_TIME_SYNC else anchor.session_time_ms
    jitter_ms = None
    guard_violation = None
    if session_ms is not None and pkt_type != PKT_TIME_SYNC:
        if pkt_type is None:
            slot_node_id = _VIRTUAL_BASE_NODE_ID if rh_owner_node_id == 0 else rh_owner_node_id
        elif pkt_type in _BASE_SLOTTED_TYPES:
            slot_node_id = _VIRTUAL_BASE_NODE_ID
        else:
            slot_node_id = node_id
        if slot_node_id is not None:
            jitter_ms, guard_violation = _slot_timing(slot_node_id, session_ms, num_slots)

    event = {
        "wall_t": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
        "sniffer_t_ms": t_ms,
        "session_ms": session_ms,
        "pkt_type": pkt_type_name,
        "node_id": node_id,
        "seq": seq,
        "rssi": rssi,
        "snr": snr,
        "jitter_ms": round(jitter_ms, 1) if jitter_ms is not None else None,
        "guard_violation": guard_violation,
        "anchored": anchor.anchored,
        "num_slots": num_slots,
        "payload_hex": payload_hex,
        "rh_to": rh_to,
        "rh_from": rh_from,
        "rh_id": rh_id,
        "rh_flags": rh_flags,
        "rh_is_ack": rh_is_ack,
        "rh_is_retry": rh_is_retry,
        "rh_owner_node_id": rh_owner_node_id,
    }
    event.update(extra)
    event["_session_changed"] = session_changed
    return event


def run_sniffer(
    cfg: SnifferConfig,
    live_state: LiveState,
    log_fn: Callable[..., None] | None = None,
) -> int:
    """Run the sniffer ingest loop. Intended to be run in a daemon thread
    alongside the base-station ingest loop (see web_service.run_web)."""
    if log_fn is None:
        log_fn = lambda msg, node_id=None, source="sniffer", kind="other": print(msg)  # noqa: E731

    # Every log line from this loop is tagged source="sniffer" so the Live
    # Log page can filter to just sniffer activity, the same way it filters
    # by node — see LiveState.push_log / live_log_page.js.
    def slog(msg: str, node_id: Optional[int] = None, kind: str = "other") -> None:
        log_fn(msg, node_id, source="sniffer", kind=kind)

    anchor = _SyncAnchor()
    slog(f"[SNIFFER] Listening on {cfg.port} @ {cfg.baud}, num_slots={cfg.num_slots}")

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
                        slog(f"[SNIFFER] {msg.get('message') or msg}")
                    continue

                event = _decode_rx_event(msg, anchor, cfg.num_slots)
                if event is None:
                    continue

                if event.pop("_session_changed", False):
                    live_state.reset_sniffer_stats()
                    slog("[SNIFFER] New base station session detected — sniffer stats reset")

                live_state.push_sniffer_event(event)
                rx_kind = {"STATUS": "status", "BUNDLE": "bundle"}.get(event["pkt_type"], "other")
                # Bare RadioHead frames (RH_ACK/RH_RAW) carry no SmartFires
                # header, so node_id is None — fall back to rh_owner_node_id
                # (the TDMA slot attribution computed above) so the Live Log
                # page's per-node filter can still bucket these correctly.
                attributed_node_id = (
                    event["node_id"] if event["node_id"] is not None else event["rh_owner_node_id"]
                )
                slog(
                    f"[SNIFFER-RX] type={event['pkt_type']} node={attributed_node_id} "
                    f"rssi={event['rssi']} snr={event['snr']} jitter={event['jitter_ms']}",
                    attributed_node_id,
                    kind=rx_kind,
                )
    except serial.SerialException as exc:
        print(f"[SNIFFER][FATAL] {exc}", file=sys.stderr)
        slog(f"[SNIFFER] serial error: {exc}")
        return 1
    except KeyboardInterrupt:
        slog("[SNIFFER] Stopped by user.")
    return 0
