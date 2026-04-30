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
PKT_GPS        = PKT_STATUS  # legacy alias
PKT_ACK_SUMMARY = 0x07

# ---------- struct formats (little-endian, packed) ----------

# PktHeader: magic(1) pkt_type(1) node_id(1) seq(1)  →  4 bytes
HEADER_FMT  = "<BBBB"
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 4

# FullStatePayload:
#   session_time(u32) sensor_flags(u16)
#   wind_cms(u16) temp_cdegc(i16) humidity_cpct(u16)
#   pm1_0_ug10(u16) pm2_5_ug10(u16) pm4_0_ug10(u16) pm10_ug10(u16)
#   →  20 bytes
FULL_STATE_FMT  = "<IHHhHHHHH"
FULL_STATE_SIZE = struct.calcsize(FULL_STATE_FMT)  # 20

# StatusPayload: lat_e7(i32) lon_e7(i32) battery_mv(u16) battery_pct(u8) flags(u8)
STATUS_PAYLOAD_FMT  = "<iiHBB"
STATUS_PAYLOAD_SIZE = struct.calcsize(STATUS_PAYLOAD_FMT)  # 12
STATUS_GPS_VALID = 0x01

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

STATUS_LORA_SIZE      = HEADER_SIZE + STATUS_PAYLOAD_SIZE + 1
LORA_PAYLOAD_SIZE     = HEADER_SIZE + FULL_STATE_SIZE + 1
LORA_BUNDLE_MAX_SIZE  = HEADER_SIZE + FULL_STATE_SIZE + 1 + BUNDLE_MAX_DELTAS * DELTA_SIZE + 1
TIME_SYNC_LORA_SIZE   = HEADER_SIZE + TIME_SYNC_PAYLOAD_SIZE + 1
ACK_SUMMARY_LORA_SIZE = HEADER_SIZE + ACK_SUMMARY_PAYLOAD_SIZE + 1

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
    hdr = struct.pack(HEADER_FMT, PKT_MAGIC, PKT_TIME_SYNC, 0, seq & 0xFF)
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
    hdr = struct.pack(HEADER_FMT, PKT_MAGIC, PKT_ACK_SUMMARY, 0, seq & 0xFF)
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
) -> dict:
    return {
        "node_id": node_id,
        "seq": seq,
        "session_time_ms": session_time,
        "uptime_ms": session_time,
        "sensor_flags": sensor_flags,
        "wind_mps": round(wind_cms / 100.0, 2),
        "temp_c": round(temp_cdegc / 100.0, 2),
        "humidity_pct": round(humidity_cpct / 100.0, 2),
        "pm1_0_ug_m3": round(pm1_0_ug10 / 10.0, 1),
        "pm2_5_ug_m3": round(pm2_5_ug10 / 10.0, 1),
        "pm4_0_ug_m3": round(pm4_0_ug10 / 10.0, 1),
        "pm10_ug_m3": round(pm10_ug10 / 10.0, 1),
        "rssi": rssi,
    }


# ---------- decode ----------


def decode_gps(raw_lora_payload: bytes, rssi: Optional[int] = None) -> Optional[dict]:
    """Decode a raw LoRa STATUS payload and return GPS fields when valid."""
    if len(raw_lora_payload) < STATUS_LORA_SIZE:
        return None
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return None

    magic, pkt_type, node_id, seq = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
    if magic != PKT_MAGIC or pkt_type != PKT_STATUS:
        return None

    lat_e7, lon_e7, _battery_mv, _battery_pct, flags = struct.unpack_from(
        STATUS_PAYLOAD_FMT,
        raw_lora_payload,
        HEADER_SIZE,
    )
    if (flags & STATUS_GPS_VALID) == 0:
        return None

    return {
        "node_id": node_id,
        "seq": seq,
        "lat": round(lat_e7 / 1e7, 7),
        "lon": round(lon_e7 / 1e7, 7),
        "rssi": rssi,
    }

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

    magic, pkt_type, node_id, seq = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
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
    )


def decode_bundle(raw_lora_payload: bytes, rssi: Optional[int] = None) -> list[dict]:
    """Decode a PKT_BUNDLE LoRa payload into reference + expanded deltas."""
    min_size = HEADER_SIZE + FULL_STATE_SIZE + 1 + 1  # +1 CRC
    if len(raw_lora_payload) < min_size:
        return []
    if crc8(raw_lora_payload[:-1]) != raw_lora_payload[-1]:
        return []

    magic, pkt_type, node_id, seq = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)
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
        )
    ]

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
            _flags,
        ) = struct.unpack_from(DELTA_FMT, raw_lora_payload, offset)
        offset += DELTA_SIZE

        dt_ms = dt_ticks_250ms * 250
        results.append(
            _full_state_fields(
                node_id,
                seq,
                session_time + dt_ms,
                sensor_flags,
                d_wind_cms,
                temp_cdegc + (d_temp_deci_c * 10),
                humidity_cpct + (d_humidity_0p2pct * 20),
                pm1_0_ug10 + (d_pm1_0_ug * 10),
                pm2_5_ug10 + d_pm2_5_ug10,
                pm4_0_ug10 + (d_pm4_0_ug * 10),
                pm10_ug10 + d_pm10_ug10,
                rssi,
            )
        )

    return results
