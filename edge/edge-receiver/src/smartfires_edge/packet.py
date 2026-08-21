"""SmartFires binary packet definitions — Python mirror of BinaryPacket.h."""

import struct
from typing import Optional

# ---------- frame constants ----------

FRAME_M0 = 0xAA
FRAME_M1 = 0x55
PKT_MAGIC = 0xA5

# ---------- packet types ----------

PKT_FULL_STATE = 0x01
PKT_HEARTBEAT  = 0x02
PKT_TIME_SYNC  = 0x03
PKT_BUNDLE     = 0x04
PKT_STATUS     = 0x05
PKT_AWAKEN     = 0x06
PKT_GPS        = PKT_STATUS  # legacy alias
PKT_ACK_SUMMARY = 0x07
# Timed duty-cycle active-window edges. Carried on their own frames rather than
# as flags on the sample stream — see decode_window_marker().
PKT_WINDOW_BEGIN = 0x08
PKT_WINDOW_END = 0x09
PKT_CMD_CALIBRATE = 0x10
PKT_CMD_RESET = 0x11
PKT_CALIBRATION_DATA = 0x12
PKT_CMD_ACK = 0x13
# Base -> Jetson only, never sent over LoRa — see BinaryPacket.h's PKT_DEBUG_LOG.
PKT_DEBUG_LOG = 0x14
# Base -> one node. The base station owns the dynamic TX power decision; the
# Jetson only encodes these for manual/bench use and passively decodes them for
# monitoring. It is not a participant in the control loop — see
# documentation/Pending_Plans/DYNAMIC_TX_POWER.md.
PKT_CMD_SET_TX_POWER = 0x15

# sensor_flags bits (SensorSnapshot.h / CLAUDE.md): which sensors had a valid
# reading this sample. When a bit is clear the corresponding fields carry a
# node-side hold-last-good substitute (or, pre-fix firmware, a -1.0 placeholder)
# — either way they are not real measurements and are nulled on decode.
SENSOR_FLAG_WIND  = 0x01
SENSOR_FLAG_SHT31 = 0x02
SENSOR_FLAG_GPS   = 0x04
SENSOR_FLAG_IMU   = 0x08
SENSOR_FLAG_SPS30 = 0x10

# ---------- struct formats (little-endian, packed) ----------

# PktHeader: magic(1) pkt_type(1) node_id(1) seq(1) flags(1)  →  5 bytes
HEADER_FMT  = "<BBBBB"
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 5

# PktHeader.flags bits (BinaryPacket.h PKT_FLAG_*). RETX marks an app-layer
# retransmission: the same samples were already sent once and this copy is a
# replay because no ACK_SUMMARY covered them. Rows carrying it are duplicates of
# earlier rows (same node_id + seq), not new observations — deduplicate on
# (node_id, seq) before computing sample rates.
#
# 0x01/0x02 were WINDOW_FIRST/WINDOW_LAST, set on the bundles at a Timed
# window's edges. They are retired in favour of the PKT_WINDOW_BEGIN/
# PKT_WINDOW_END frames; the window_first/window_last CSV columns are still
# produced, but the ingest layer now derives them from those frames
# (see ingest_service.py's window tracking) instead of reading them off a bundle.
PKT_FLAG_RETX = 0x04

# Legacy pre-flags header (4 bytes, no flags byte). Only still recognised on
# AWAKEN — see decode_awaken().
LEGACY_HEADER_SIZE = 4

# FullStatePayload:
#   session_time(u32) sensor_flags(u16)
#   wind_cms(u16) temp_cdegc(i16) humidity_cpct(u16)
#   pm1_0_ug10(u16) pm2_5_ug10(u16) pm4_0_ug10(u16) pm10_ug10(u16)
#   →  20 bytes
FULL_STATE_FMT  = "<IHHhHHHHH"
FULL_STATE_SIZE = struct.calcsize(FULL_STATE_FMT)  # 20

# AwakenPayload (legacy, pre-diagnostics): uid_hash(u32)
AWAKEN_PAYLOAD_FMT = "<I"
AWAKEN_PAYLOAD_SIZE = struct.calcsize(AWAKEN_PAYLOAD_FMT)  # 4
# AwakenPayload (current): uid_hash(u32) reset_cause(u8) hang_zone(u8)
AWAKEN_PAYLOAD_FMT_V2 = "<IBB"
AWAKEN_PAYLOAD_SIZE_V2 = struct.calcsize(AWAKEN_PAYLOAD_FMT_V2)  # 6

# HangZone enum mirror (platform/ResetDiagnostics.h) — keep in sync, append only.
HANG_ZONE_NAMES = {
    0: "UNKNOWN",
    1: "BOOT",
    2: "RADIO_TX",
    3: "I2C_SHT31",
    4: "I2C_GPS",
    5: "I2C_IMU",
    6: "UART_SPS30",
    7: "LOOP_IDLE",
}

# SAMD21 PM->RCAUSE bit names (raw reset_cause byte), most-significant cause first.
_RCAUSE_BITS = [
    (0x40, "SYST"),  # system reset request
    (0x20, "WDT"),   # watchdog
    (0x10, "EXT"),   # external reset pin
    (0x04, "BOD33"), # 3.3V brownout
    (0x02, "BOD12"), # 1.2V core brownout
    (0x01, "POR"),   # power-on reset
]


def reset_cause_names(reset_cause):
    """Decode a raw SAMD21 RCAUSE byte into a list of set-cause names."""
    if reset_cause is None:
        return None
    names = [name for bit, name in _RCAUSE_BITS if reset_cause & bit]
    return names or ["NONE"]

# StatusPayload: lat_e7(i32) lon_e7(i32) battery_mv(u16) battery_pct(u8) flags(u8)
#                heading_deg_x10(u16) heading_accuracy(u16)
#                retx_total(u16) fail_total(u16) tx_power_dbm(i8)
STATUS_PAYLOAD_FMT  = "<iiHBBHHHHb"
STATUS_PAYLOAD_SIZE = struct.calcsize(STATUS_PAYLOAD_FMT)  # 21
STATUS_GPS_VALID  = 0x01
STATUS_BATT_VALID = 0x02
STATUS_IMU_VALID  = 0x04
# Not a validity flag — the node's TX power control mode. Set means an operator
# pinned the level (TX_POWER_MODE_STATIC); clear means the base's loop owns it.
STATUS_TX_POWER_STATIC = 0x08

# Older STATUS layouts, each parsed against its own length. New fields are only
# ever appended to StatusPayload, so a shorter frame is a strict prefix of a
# longer one and the absent fields decode as None.
#
#   v2: before tx_power_dbm (dynamic-tx-power)      → 26 bytes on air
#   v1: before retx_total/fail_total (link stats)   → 22 bytes on air
_V2_STATUS_PAYLOAD_FMT   = "<iiHBBHHHH"
_V2_STATUS_LORA_SIZE     = HEADER_SIZE + struct.calcsize(_V2_STATUS_PAYLOAD_FMT) + 1  # 26
_LEGACY_STATUS_PAYLOAD_FMT  = "<iiHBBHH"
_LEGACY_STATUS_LORA_SIZE    = HEADER_SIZE + struct.calcsize(_LEGACY_STATUS_PAYLOAD_FMT) + 1  # 22

# CmdCalibratePayload: node_id(u8) duration_s(u8)
CMD_CALIBRATE_PAYLOAD_FMT = "<BB"
CMD_CALIBRATE_PAYLOAD_SIZE = struct.calcsize(CMD_CALIBRATE_PAYLOAD_FMT)  # 2

# CmdResetPayload: node_id(u8) reset_type(u8)
CMD_RESET_PAYLOAD_FMT = "<BB"
CMD_RESET_PAYLOAD_SIZE = struct.calcsize(CMD_RESET_PAYLOAD_FMT)  # 2

# CmdSetTxPowerPayload: node_id(u8) tx_power_dbm(i8) mode(u8)
CMD_SET_TX_POWER_PAYLOAD_FMT = "<BbB"
CMD_SET_TX_POWER_PAYLOAD_SIZE = struct.calcsize(CMD_SET_TX_POWER_PAYLOAD_FMT)  # 3

# Per-node TX power control mode (BinaryPacket.h TX_POWER_MODE_*).
#   DYNAMIC — the base station's control loop owns this node's power.
#   STATIC  — an operator pinned the level; the loop leaves it alone.
# STATIC is an override, not a safety state: a node that loses contact with the
# base for syncStaleMs reverts to DYNAMIC at baseline like everything else.
TX_POWER_MODE_DYNAMIC = 0x00
TX_POWER_MODE_STATIC = 0x01

# CmdAckPayload: cmd_type(u8) uid_hash(u32) status(u8)
CMD_ACK_PAYLOAD_FMT = "<BIB"
CMD_ACK_PAYLOAD_SIZE = struct.calcsize(CMD_ACK_PAYLOAD_FMT)  # 6

# DeltaPayload (12-byte compact format)
DELTA_FMT  = "<BHbbbhbhB"
DELTA_SIZE = struct.calcsize(DELTA_FMT)  # 12
BUNDLE_MAX_DELTAS = 14

# TimeSyncPayload: session_id(u32) session_time_ms(u32)
TIME_SYNC_PAYLOAD_FMT  = "<II"
TIME_SYNC_PAYLOAD_SIZE = struct.calcsize(TIME_SYNC_PAYLOAD_FMT)  # 8

# AckSummaryPayload: node_id(u8) ack_base_seq(u8) ack_mask(u16)
ACK_SUMMARY_PAYLOAD_FMT  = "<BBH"
ACK_SUMMARY_PAYLOAD_SIZE = struct.calcsize(ACK_SUMMARY_PAYLOAD_FMT)  # 4

# WindowMarkerPayload:
#   session_time_ms(u32) planned_sleep_ms(u32) window_id(u16) sample_count(u8)
# planned_sleep_ms and sample_count are only populated on PKT_WINDOW_END.
WINDOW_MARKER_PAYLOAD_FMT  = "<IIHB"
WINDOW_MARKER_PAYLOAD_SIZE = struct.calcsize(WINDOW_MARKER_PAYLOAD_FMT)  # 11

STATUS_LORA_SIZE      = HEADER_SIZE + STATUS_PAYLOAD_SIZE + 1
LORA_PAYLOAD_SIZE     = HEADER_SIZE + FULL_STATE_SIZE + 1
LORA_BUNDLE_MAX_SIZE  = HEADER_SIZE + FULL_STATE_SIZE + 1 + BUNDLE_MAX_DELTAS * DELTA_SIZE + 1
TIME_SYNC_LORA_SIZE   = HEADER_SIZE + TIME_SYNC_PAYLOAD_SIZE + 1
ACK_SUMMARY_LORA_SIZE = HEADER_SIZE + ACK_SUMMARY_PAYLOAD_SIZE + 1
WINDOW_MARKER_LORA_SIZE = HEADER_SIZE + WINDOW_MARKER_PAYLOAD_SIZE + 1  # 17
# The legacy AWAKEN frame predates the header flags byte, so it sizes off the
# 4-byte header, not HEADER_SIZE.
AWAKEN_LORA_SIZE      = LEGACY_HEADER_SIZE + AWAKEN_PAYLOAD_SIZE + 1  # 9 (legacy)
AWAKEN_LORA_SIZE_V2   = HEADER_SIZE + AWAKEN_PAYLOAD_SIZE_V2 + 1      # 12 (current)
CMD_CALIBRATE_LORA_SIZE = HEADER_SIZE + CMD_CALIBRATE_PAYLOAD_SIZE + 1
CMD_RESET_LORA_SIZE   = HEADER_SIZE + CMD_RESET_PAYLOAD_SIZE + 1
CMD_SET_TX_POWER_LORA_SIZE = HEADER_SIZE + CMD_SET_TX_POWER_PAYLOAD_SIZE + 1  # 9
CMD_ACK_LORA_SIZE     = HEADER_SIZE + CMD_ACK_PAYLOAD_SIZE + 1

BASE_FRAME_MIN_DATA_LEN = 5
BASE_FRAME_MAX_DATA_LEN = 1 + LORA_BUNDLE_MAX_SIZE

TIME_SYNC_DATA_LEN  = TIME_SYNC_LORA_SIZE
TIME_SYNC_FRAME_LEN = 2 + 1 + TIME_SYNC_DATA_LEN + 1


# ---------- CRC-8/MAXIM (polynomial 0x31) ----------

def crc8(data: bytes) -> int:
    """Compute CRC-8/MAXIM over data. Matches the C++ implementation in BinaryPacket.h."""
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def encode_time_sync_frame(session_id: int, session_time_ms: int, seq: int = 0) -> bytes:
    """Encode a TIME_SYNC UART frame from Jetson to the base Feather."""
    hdr = struct.pack(HEADER_FMT, PKT_MAGIC, PKT_TIME_SYNC, 0, seq & 0xFF, 0)
    ts = struct.pack(
        TIME_SYNC_PAYLOAD_FMT,
        session_id & 0xFFFFFFFF,
        session_time_ms & 0xFFFFFFFF,
    )
    lora_payload_no_crc = hdr + ts
    lora_crc = crc8(lora_payload_no_crc)
    lora_payload = lora_payload_no_crc + bytes([lora_crc])
    data_len = len(lora_payload)  # TIME_SYNC_LORA_SIZE = 13
    frame_crc = crc8(bytes([data_len]) + lora_payload)
    return bytes([FRAME_M0, FRAME_M1, data_len]) + lora_payload + bytes([frame_crc])


def encode_ack_summary_frame(node_id: int, ack_base_seq: int, ack_mask: int, seq: int = 0) -> bytes:
    """Encode an ACK_SUMMARY UART frame for base-station forwarding."""
    hdr = struct.pack(HEADER_FMT, PKT_MAGIC, PKT_ACK_SUMMARY, 0, seq & 0xFF, 0)
    ack = struct.pack(
        ACK_SUMMARY_PAYLOAD_FMT,
        node_id & 0xFF,
        ack_base_seq & 0xFF,
        ack_mask & 0xFFFF,
    )
    lora_payload_no_crc = hdr + ack
    lora_crc = crc8(lora_payload_no_crc)
    lora_payload = lora_payload_no_crc + bytes([lora_crc])
    data_len = len(lora_payload)  # ACK_SUMMARY_LORA_SIZE = 9
    frame_crc = crc8(bytes([data_len]) + lora_payload)
    return bytes([FRAME_M0, FRAME_M1, data_len]) + lora_payload + bytes([frame_crc])


def encode_cmd_calibrate_frame(node_id: int, duration_s: int = 60, seq: int = 0) -> bytes:
    """Encode a CMD_CALIBRATE UART frame for base-station forwarding."""
    hdr = struct.pack(HEADER_FMT, PKT_MAGIC, PKT_CMD_CALIBRATE, 0, seq & 0xFF, 0)
    cmd = struct.pack(CMD_CALIBRATE_PAYLOAD_FMT, node_id & 0xFF, duration_s & 0xFF)
    lora_payload_no_crc = hdr + cmd
    lora_crc = crc8(lora_payload_no_crc)
    lora_payload = lora_payload_no_crc + bytes([lora_crc])
    data_len = len(lora_payload)  # CMD_CALIBRATE_LORA_SIZE = 7
    frame_crc = crc8(bytes([data_len]) + lora_payload)
    return bytes([FRAME_M0, FRAME_M1, data_len]) + lora_payload + bytes([frame_crc])


def encode_cmd_reset_frame(node_id: int, reset_type: int = 0, seq: int = 0) -> bytes:
    """Encode a CMD_RESET UART frame for base-station forwarding."""
    hdr = struct.pack(HEADER_FMT, PKT_MAGIC, PKT_CMD_RESET, 0, seq & 0xFF, 0)
    cmd = struct.pack(CMD_RESET_PAYLOAD_FMT, node_id & 0xFF, reset_type & 0xFF)
    lora_payload_no_crc = hdr + cmd
    lora_crc = crc8(lora_payload_no_crc)
    lora_payload = lora_payload_no_crc + bytes([lora_crc])
    data_len = len(lora_payload)  # CMD_RESET_LORA_SIZE = 7
    frame_crc = crc8(bytes([data_len]) + lora_payload)
    return bytes([FRAME_M0, FRAME_M1, data_len]) + lora_payload + bytes([frame_crc])


def encode_cmd_set_tx_power_frame(
    node_id: int,
    tx_power_dbm: int,
    mode: int = TX_POWER_MODE_DYNAMIC,
    seq: int = 0,
) -> bytes:
    """Encode a CMD_SET_TX_POWER UART frame for base-station forwarding.

    The base station is the decision-maker for dynamic TX power
    (documentation/Pending_Plans/DYNAMIC_TX_POWER.md). This frame is the
    operator override on top of that: sending mode=STATIC pins a node at a
    level and takes it out of the loop, mode=DYNAMIC hands it back.

    tx_power_dbm is always ABSOLUTE, never a delta. The dashboard's
    increase/decrease buttons resolve to an absolute value client-side from the
    node's last reported StatusPayload.tx_power_dbm, so a stale reading can only
    ever produce a slightly-off absolute target — never a runaway. Do not add a
    relative variant; see CmdSetTxPowerPayload in BinaryPacket.h.

    tx_power_dbm is a *request* — the node clamps it to its own safe range
    (NetworkConfig::kMinTxPowerDbm..kMaxTxPowerDbm) before applying. Read
    StatusPayload.tx_power_dbm back to see what actually landed.
    """
    hdr = struct.pack(HEADER_FMT, PKT_MAGIC, PKT_CMD_SET_TX_POWER, 0, seq & 0xFF, 0)
    cmd = struct.pack(
        CMD_SET_TX_POWER_PAYLOAD_FMT, node_id & 0xFF, tx_power_dbm, mode & 0xFF
    )
    lora_payload_no_crc = hdr + cmd
    lora_crc = crc8(lora_payload_no_crc)
    lora_payload = lora_payload_no_crc + bytes([lora_crc])
    data_len = len(lora_payload)  # CMD_SET_TX_POWER_LORA_SIZE = 9
    frame_crc = crc8(bytes([data_len]) + lora_payload)
    return bytes([FRAME_M0, FRAME_M1, data_len]) + lora_payload + bytes([frame_crc])


def decode_cmd_set_tx_power(raw_lora_payload: bytes) -> Optional[dict]:
    """Passively decode a CMD_SET_TX_POWER frame (sniffer/monitoring)."""
    if len(raw_lora_payload) < CMD_SET_TX_POWER_LORA_SIZE:
        return None
    frame = raw_lora_payload[:CMD_SET_TX_POWER_LORA_SIZE]
    if crc8(frame[:-1]) != frame[-1]:
        return None

    magic, pkt_type, hdr_node_id, seq, _hdr_flags = struct.unpack_from(HEADER_FMT, frame, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_CMD_SET_TX_POWER:
        return None

    node_id, tx_power_dbm, mode = struct.unpack_from(
        CMD_SET_TX_POWER_PAYLOAD_FMT, frame, HEADER_SIZE)
    return {
        "hdr_node_id": hdr_node_id,
        "seq": seq,
        "node_id": node_id,
        "tx_power_dbm": tx_power_dbm,
        "mode": mode,
        "mode_name": "STATIC" if mode == TX_POWER_MODE_STATIC else "DYNAMIC",
    }


def _full_state_fields(
    node_id: int,
    seq: int,
    session_time: int,
    sensor_flags: int,
    wind_cms: int,
    temp_cdegc: int,
    humidity_cpct: int,
    pm1_0_ug10: int,
    pm2_5_ug10: int,
    pm4_0_ug10: int,
    pm10_ug10: int,
    rssi: Optional[int],
    delta_flags: Optional[int] = None,
    pkt_flags: int = 0,
) -> dict:
    wind_valid = (sensor_flags & SENSOR_FLAG_WIND) != 0
    sht31_valid = (sensor_flags & SENSOR_FLAG_SHT31) != 0
    sps30_valid = (sensor_flags & SENSOR_FLAG_SPS30) != 0
    return {
        "node_id": node_id,
        "seq": seq,
        "session_time_ms": session_time,
        "uptime_ms": session_time,
        "sensor_flags": sensor_flags,
        "wind_mps": round(wind_cms / 100.0, 2) if wind_valid else None,
        "temp_c": round(temp_cdegc / 100.0, 2) if sht31_valid else None,
        "humidity_pct": round(humidity_cpct / 100.0, 2) if sht31_valid else None,
        "pm1_0_ug_m3": round(pm1_0_ug10 / 10.0, 1) if sps30_valid else None,
        "pm2_5_ug_m3": round(pm2_5_ug10 / 10.0, 1) if sps30_valid else None,
        "pm4_0_ug_m3": round(pm4_0_ug10 / 10.0, 1) if sps30_valid else None,
        "pm10_ug_m3": round(pm10_ug10 / 10.0, 1) if sps30_valid else None,
        "rssi": rssi,
        "delta_flags": delta_flags,
        # PktHeader.flags of the bundle this sample arrived in.
        "pkt_flags": pkt_flags,
        # window_first/window_last/window_id are filled in by the ingest layer
        # from the PKT_WINDOW_BEGIN/PKT_WINDOW_END frames that bracket this
        # bundle; the decoder cannot know them from the bundle alone. Defaulted
        # here so the CSV row shape is the same whether or not window tracking
        # is active (Continuous mode never emits markers at all).
        "window_first": 0,
        "window_last": 0,
        "window_id": None,
        "retx": 1 if pkt_flags & PKT_FLAG_RETX else 0,
    }


# ---------- decode ----------


def decode_gps(raw_lora_payload: bytes, rssi: Optional[int] = None) -> Optional[dict]:
    """Decode a raw LoRa STATUS payload and return GPS fields when valid."""
    n = len(raw_lora_payload)
    if n < _LEGACY_STATUS_LORA_SIZE:
        return None
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return None

    magic, pkt_type, node_id, seq, hdr_flags = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_STATUS:
        return None

    fmt = STATUS_PAYLOAD_FMT if n >= STATUS_LORA_SIZE else _LEGACY_STATUS_PAYLOAD_FMT
    lat_e7, lon_e7, _battery_mv, _battery_pct, flags = struct.unpack_from(fmt, raw_lora_payload, HEADER_SIZE)[:5]
    if (flags & STATUS_GPS_VALID) == 0:
        return None

    return {
        "node_id": node_id,
        "seq": seq,
        "lat": round(lat_e7 / 1e7, 7),
        "lon": round(lon_e7 / 1e7, 7),
        "rssi": rssi,
    }


def decode_status(raw_lora_payload: bytes, rssi: Optional[int] = None) -> Optional[dict]:
    """Decode a raw LoRa STATUS payload into GPS + battery + link-stats fields.

    Length-adaptive across three firmware generations: the current 27-byte
    format, the 26-byte one before tx_power_dbm, and the legacy 22-byte one
    before retx_total/fail_total. Fields a given generation doesn't carry come
    back as None rather than a fabricated default, so a caller can tell
    "firmware too old to report this" from a real value.
    """
    n = len(raw_lora_payload)
    if n < _LEGACY_STATUS_LORA_SIZE:
        return None
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return None

    magic, pkt_type, node_id, seq, hdr_flags = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_STATUS:
        return None

    tx_power_dbm = None
    if n >= STATUS_LORA_SIZE:
        lat_e7, lon_e7, battery_mv, battery_pct, flags, heading_deg_x10, heading_accuracy, retx_total, fail_total, tx_power_dbm = \
            struct.unpack_from(STATUS_PAYLOAD_FMT, raw_lora_payload, HEADER_SIZE)
    elif n >= _V2_STATUS_LORA_SIZE:
        lat_e7, lon_e7, battery_mv, battery_pct, flags, heading_deg_x10, heading_accuracy, retx_total, fail_total = \
            struct.unpack_from(_V2_STATUS_PAYLOAD_FMT, raw_lora_payload, HEADER_SIZE)
    else:
        lat_e7, lon_e7, battery_mv, battery_pct, flags, heading_deg_x10, heading_accuracy = \
            struct.unpack_from(_LEGACY_STATUS_PAYLOAD_FMT, raw_lora_payload, HEADER_SIZE)
        retx_total = None
        fail_total = None

    gps_valid  = (flags & STATUS_GPS_VALID)  != 0
    batt_valid = (flags & STATUS_BATT_VALID) != 0
    imu_valid  = (flags & STATUS_IMU_VALID)  != 0
    # None (not False) on firmware too old to report a mode at all, so a caller
    # can tell "not supported" from "dynamic".
    tx_power_static = (
        ((flags & STATUS_TX_POWER_STATIC) != 0) if tx_power_dbm is not None else None
    )

    return {
        "node_id": node_id,
        "seq": seq,
        "rssi": rssi,
        "flags": flags,
        "gps_valid": gps_valid,
        "battery_valid": batt_valid,
        "imu_valid": imu_valid,
        "lat": round(lat_e7 / 1e7, 7) if gps_valid else "",
        "lon": round(lon_e7 / 1e7, 7) if gps_valid else "",
        "battery_mv": battery_mv if batt_valid else "",
        "battery_pct": battery_pct if batt_valid else "",
        "heading_deg": round(heading_deg_x10 / 10.0, 1) if imu_valid else "",
        "heading_accuracy": heading_accuracy if imu_valid else "",
        "retx_total": retx_total,
        "fail_total": fail_total,
        # The node's own applied radio TX power, not the base's record of what
        # it last commanded — see StatusPayload::tx_power_dbm in BinaryPacket.h.
        # None on firmware predating dynamic-tx-power.
        "tx_power_dbm": tx_power_dbm,
        "tx_power_static": tx_power_static,
        "tx_power_mode": (
            None if tx_power_static is None else ("STATIC" if tx_power_static else "DYNAMIC")
        ),
    }


def decode_awaken(raw_lora_payload: bytes, rssi: Optional[int] = None) -> Optional[dict]:
    # Length-adaptive: accept the current 12-byte frame (5-byte header with the
    # flags byte, uid_hash + reset_cause + hang_zone) and the legacy 9-byte frame
    # (4-byte header, uid_hash only), each parsed against its own header size.
    #
    # A node old enough to send the legacy frame also encodes BUNDLE/STATUS with
    # the 4-byte header, which this decoder cannot read — accepting its AWAKEN
    # only keeps the handshake visible, it does not make the node usable.
    n = len(raw_lora_payload)
    if n >= AWAKEN_LORA_SIZE_V2:
        frame_len, header_size, has_diag = AWAKEN_LORA_SIZE_V2, HEADER_SIZE, True
    elif n >= AWAKEN_LORA_SIZE:
        frame_len, header_size, has_diag = AWAKEN_LORA_SIZE, LEGACY_HEADER_SIZE, False
    else:
        return None

    frame = raw_lora_payload[:frame_len]
    if crc8(frame[:-1]) != frame[-1]:
        return None

    if has_diag:
        magic, pkt_type, node_id, seq, _hdr_flags = struct.unpack_from(
            HEADER_FMT, frame, 0)
    else:
        magic, pkt_type, node_id, seq = struct.unpack_from("<BBBB", frame, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_AWAKEN:
        return None

    reset_cause = None
    hang_zone = None
    if has_diag:
        uid_hash, reset_cause, hang_zone = struct.unpack_from(
            AWAKEN_PAYLOAD_FMT_V2, frame, header_size)
    else:
        (uid_hash,) = struct.unpack_from(AWAKEN_PAYLOAD_FMT, frame, header_size)

    return {
        "node_id": node_id,
        "seq": seq,
        "rssi": rssi,
        "uid_hash": uid_hash,
        "reset_cause": reset_cause,
        "reset_cause_names": reset_cause_names(reset_cause),
        "hang_zone": hang_zone,
        "hang_zone_name": (HANG_ZONE_NAMES.get(hang_zone) if hang_zone is not None
                           else None),
    }


def decode_time_sync(raw_lora_payload: bytes, rssi: Optional[int] = None) -> Optional[dict]:
    """Decode a PKT_TIME_SYNC LoRa broadcast (base station -> nodes)."""
    if len(raw_lora_payload) < TIME_SYNC_LORA_SIZE:
        return None
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return None

    magic, pkt_type, node_id, seq, hdr_flags = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_TIME_SYNC:
        return None

    session_id, session_time_ms = struct.unpack_from(TIME_SYNC_PAYLOAD_FMT, raw_lora_payload, HEADER_SIZE)
    return {
        "node_id": node_id,
        "seq": seq,
        "rssi": rssi,
        "session_id": session_id,
        "session_time_ms": session_time_ms,
    }


def decode_ack_summary(raw_lora_payload: bytes, rssi: Optional[int] = None) -> Optional[dict]:
    """Decode a PKT_ACK_SUMMARY LoRa broadcast (base station -> target node).

    The header's node_id is 0 (broadcast/command convention) — the actual
    target node lives in the payload's own node_id field.
    """
    if len(raw_lora_payload) < ACK_SUMMARY_LORA_SIZE:
        return None
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return None

    magic, pkt_type, hdr_node_id, seq, hdr_flags = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_ACK_SUMMARY:
        return None

    node_id, ack_base_seq, ack_mask = struct.unpack_from(ACK_SUMMARY_PAYLOAD_FMT, raw_lora_payload, HEADER_SIZE)
    return {
        "hdr_node_id": hdr_node_id,
        "seq": seq,
        "rssi": rssi,
        "node_id": node_id,
        "ack_base_seq": ack_base_seq,
        "ack_mask": ack_mask,
    }


def decode_window_marker(
    raw_lora_payload: bytes, rssi: Optional[int] = None
) -> Optional[dict]:
    """Decode a PKT_WINDOW_BEGIN / PKT_WINDOW_END frame (node -> base).

    These bracket one Timed duty-cycle wake, replacing the retired
    WINDOW_FIRST/WINDOW_LAST header flags. They carry no telemetry ``seq`` —
    ``window_id`` is their own counter — so they must be kept out of sequence-gap
    loss accounting (see packet_loss.py).

    ``session_time_ms`` is the window edge's own instant on the shared session
    clock, which nothing else records: a bundle's last sample is taken before the
    window closes, not at the close. Attributing bundles to windows by that
    timestamp rather than by arrival order survives packet loss and the
    reordering a retransmission introduces.
    """
    if len(raw_lora_payload) < WINDOW_MARKER_LORA_SIZE:
        return None
    payload = raw_lora_payload[:WINDOW_MARKER_LORA_SIZE]
    if crc8(payload[:-1]) != payload[-1]:
        return None

    magic, pkt_type, node_id, seq, hdr_flags = struct.unpack_from(HEADER_FMT, payload, 0)
    if magic != PKT_MAGIC or pkt_type not in (PKT_WINDOW_BEGIN, PKT_WINDOW_END):
        return None

    session_time_ms, planned_sleep_ms, window_id, sample_count = struct.unpack_from(
        WINDOW_MARKER_PAYLOAD_FMT, payload, HEADER_SIZE
    )

    is_end = pkt_type == PKT_WINDOW_END
    return {
        "node_id": node_id,
        "pkt_type": pkt_type,
        "is_end": is_end,
        "window_id": window_id,
        "session_time_ms": session_time_ms,
        # Only WINDOW_END populates these; WINDOW_BEGIN sends zeros.
        "planned_sleep_ms": planned_sleep_ms if is_end else None,
        "sample_count": sample_count if is_end else None,
        "pkt_flags": hdr_flags,
        "rssi": rssi,
    }


def decode_cmd_calibrate(raw_lora_payload: bytes) -> Optional[dict]:
    if len(raw_lora_payload) < CMD_CALIBRATE_LORA_SIZE:
        return None
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return None

    magic, pkt_type, hdr_node_id, seq, hdr_flags = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_CMD_CALIBRATE:
        return None

    node_id, duration_s = struct.unpack_from(CMD_CALIBRATE_PAYLOAD_FMT, raw_lora_payload, HEADER_SIZE)
    return {
        "hdr_node_id": hdr_node_id,
        "seq": seq,
        "node_id": node_id,
        "duration_s": duration_s,
    }


def decode_cmd_reset(raw_lora_payload: bytes) -> Optional[dict]:
    if len(raw_lora_payload) < CMD_RESET_LORA_SIZE:
        return None
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return None

    magic, pkt_type, hdr_node_id, seq, hdr_flags = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_CMD_RESET:
        return None

    node_id, reset_type = struct.unpack_from(CMD_RESET_PAYLOAD_FMT, raw_lora_payload, HEADER_SIZE)
    return {
        "hdr_node_id": hdr_node_id,
        "seq": seq,
        "node_id": node_id,
        "reset_type": reset_type,
    }


def decode_cmd_ack(raw_lora_payload: bytes, rssi: Optional[int] = None) -> Optional[dict]:
    if len(raw_lora_payload) < CMD_ACK_LORA_SIZE:
        return None
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return None

    magic, pkt_type, node_id, seq, hdr_flags = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_CMD_ACK:
        return None

    cmd_type, uid_hash, status = struct.unpack_from(CMD_ACK_PAYLOAD_FMT, raw_lora_payload, HEADER_SIZE)
    return {
        "node_id": node_id,
        "seq": seq,
        "rssi": rssi,
        "cmd_type": cmd_type,
        "uid_hash": uid_hash,
        "status": status,
    }

def decode_debug_log(raw_lora_payload: bytes) -> Optional[str]:
    """Decode a PKT_DEBUG_LOG frame: PktHeader followed by a raw @SFDBG text
    line, no fixed struct and no embedded crc8 (the outer UART/USB frame's
    crc8 already covers this single Jetson hop end-to-end — see
    FramedDebugLogSink.h)."""
    if len(raw_lora_payload) < HEADER_SIZE:
        return None

    magic, pkt_type, _node_id, _seq, _hdr_flags = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_DEBUG_LOG:
        return None

    return raw_lora_payload[HEADER_SIZE:].decode("utf-8", errors="replace")


def decode_full_state(raw_lora_payload: bytes, rssi: Optional[int] = None) -> Optional[dict]:
    """
    Decode a raw LoRa payload (PktHeader + FullStatePayload) into a dict.

    Parameters
    ----------
    raw_lora_payload : bytes
        Exactly LORA_PAYLOAD_SIZE (25) bytes as received from the radio.
    rssi : int, optional
        RSSI value in dBm extracted from the UART frame.

    Returns
    -------
    dict with decoded fields, or None if the payload is invalid.
    """
    if len(raw_lora_payload) < LORA_PAYLOAD_SIZE:
        return None
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return None

    magic, pkt_type, node_id, seq, hdr_flags = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_FULL_STATE:
        return None

    (
        session_time, sensor_flags,
        wind_cms, temp_cdegc, humidity_cpct,
        pm1_0_ug10, pm2_5_ug10, pm4_0_ug10, pm10_ug10,
    ) = struct.unpack_from(FULL_STATE_FMT, raw_lora_payload, HEADER_SIZE)

    return _full_state_fields(
        node_id,
        seq,
        session_time,
        sensor_flags,
        wind_cms,
        temp_cdegc,
        humidity_cpct,
        pm1_0_ug10,
        pm2_5_ug10,
        pm4_0_ug10,
        pm10_ug10,
        rssi,
        pkt_flags=hdr_flags,
    )


def decode_bundle(raw_lora_payload: bytes, rssi: Optional[int] = None) -> list[dict]:
    """Decode a PKT_BUNDLE LoRa payload into reference + expanded deltas."""
    min_size = HEADER_SIZE + FULL_STATE_SIZE + 1 + 1  # +1 CRC
    if len(raw_lora_payload) < min_size:
        return []
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return []

    magic, pkt_type, node_id, seq, hdr_flags = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_BUNDLE:
        return []

    (
        session_time,
        sensor_flags,
        wind_cms,
        temp_cdegc,
        humidity_cpct,
        pm1_0_ug10,
        pm2_5_ug10,
        pm4_0_ug10,
        pm10_ug10,
    ) = struct.unpack_from(FULL_STATE_FMT, raw_lora_payload, HEADER_SIZE)

    offset = HEADER_SIZE + FULL_STATE_SIZE
    delta_count = raw_lora_payload[offset]
    offset += 1

    if delta_count > BUNDLE_MAX_DELTAS:
        return []
    if len(raw_lora_payload) < offset + delta_count * DELTA_SIZE + 1:  # +1 CRC byte at end
        return []

    results: list[dict] = [
        _full_state_fields(
            node_id,
            seq,
            session_time,
            sensor_flags,
            wind_cms,
            temp_cdegc,
            humidity_cpct,
            pm1_0_ug10,
            pm2_5_ug10,
            pm4_0_ug10,
            pm10_ug10,
            rssi,
            pkt_flags=hdr_flags,
        )
    ]

    current_session_time = session_time
    for _ in range(delta_count):
        (
            dt_ticks_250ms,
            d_wind_cms,
            d_temp_deci_c,
            d_humidity_0p2pct,
            d_pm1_0_ug,
            d_pm2_5_ug10,
            d_pm4_0_ug,
            d_pm10_ug10,
            delta_flags,
        ) = struct.unpack_from(DELTA_FMT, raw_lora_payload, offset)
        offset += DELTA_SIZE

        current_session_time += dt_ticks_250ms * 250
        results.append(
            _full_state_fields(
                node_id,
                seq,
                current_session_time,
                sensor_flags,
                d_wind_cms,
                temp_cdegc + (d_temp_deci_c * 10),
                humidity_cpct + (d_humidity_0p2pct * 20),
                pm1_0_ug10 + (d_pm1_0_ug * 10),
                pm2_5_ug10 + d_pm2_5_ug10,
                pm4_0_ug10 + (d_pm4_0_ug * 10),
                pm10_ug10 + d_pm10_ug10,
                rssi,
                delta_flags=delta_flags,
                pkt_flags=hdr_flags,
            )
        )

    return results
