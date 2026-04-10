"""
SmartFires binary packet definitions — Python mirror of BinaryPacket.h.

Wire formats
------------
LoRa payload (node -> base, 31 bytes):
    [PktHeader: 4][FullStatePayload: 27]

Base station UART frame (Feather base -> Jetson, 36 bytes):
    [0xAA][0x55][len=32: u8][rssi: i8][PktHeader: 4][FullStatePayload: 27][crc8]
    CRC-8/MAXIM covers the len byte + all data bytes that follow.
"""

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

# ---------- struct formats (little-endian, packed) ----------

# PktHeader: magic(1) pkt_type(1) node_id(1) seq(1)  →  4 bytes
HEADER_FMT  = "<BBBB"
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 4

# FullStatePayload:
#   session_time(u32) uptime_ms(u32) sensor_flags(u16) flame(u8)
#   wind_cms(u16) temp_cdegc(i16) humidity_cpct(u16) lidar_cm(u16)
#   lat_e7(i32) lon_e7(i32)
#   →  27 bytes
FULL_STATE_FMT  = "<IIHBHhHHii"
FULL_STATE_SIZE = struct.calcsize(FULL_STATE_FMT)  # 27

LORA_PAYLOAD_SIZE = HEADER_SIZE + FULL_STATE_SIZE   # 31

# Expected value of the len byte in a base-station UART frame
BASE_FRAME_DATA_LEN = 1 + LORA_PAYLOAD_SIZE         # 32  (rssi + 31)


# ---------- CRC-8/MAXIM (polynomial 0x31) ----------

def crc8(data: bytes) -> int:
    """Compute CRC-8/MAXIM over data. Matches the C++ implementation in BinaryPacket.h."""
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


# ---------- decode ----------

def decode_full_state(raw_lora_payload: bytes, rssi: Optional[int] = None) -> Optional[dict]:
    """
    Decode a raw LoRa payload (PktHeader + FullStatePayload) into a dict.

    Parameters
    ----------
    raw_lora_payload : bytes
        Exactly LORA_PAYLOAD_SIZE (31) bytes as received from the radio.
    rssi : int, optional
        RSSI value in dBm extracted from the UART frame.

    Returns
    -------
    dict with decoded fields, or None if the payload is invalid.
    """
    if len(raw_lora_payload) < LORA_PAYLOAD_SIZE:
        return None

    magic, pkt_type, node_id, seq = struct.unpack_from(HEADER_FMT, raw_lora_payload, 0)

    if magic != PKT_MAGIC:
        return None
    if pkt_type != PKT_FULL_STATE:
        return None

    (
        session_time, uptime_ms, sensor_flags, flame,
        wind_cms, temp_cdegc, humidity_cpct, lidar_cm,
        lat_e7, lon_e7,
    ) = struct.unpack_from(FULL_STATE_FMT, raw_lora_payload, HEADER_SIZE)

    return {
        "node_id":         node_id,
        "seq":             seq,
        "session_time_ms": session_time,
        "uptime_ms":       uptime_ms,
        "sensor_flags":    sensor_flags,
        "flame":           bool(flame),
        "wind_mps":        round(wind_cms / 100.0, 2),
        "temp_c":          round(temp_cdegc / 100.0, 2),
        "humidity_pct":    round(humidity_cpct / 100.0, 2),
        "lidar_cm":        lidar_cm,
        "lat":             round(lat_e7 / 1e7, 7),
        "lon":             round(lon_e7 / 1e7, 7),
        "rssi":            rssi,
    }
