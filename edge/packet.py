"""
SmartFires binary packet definitions — Python mirror of BinaryPacket.h.

Wire formats
------------
LoRa payload — node -> base (36 bytes):
    [PktHeader: 4][FullStatePayload: 32]

Base station UART frame — Feather base -> Jetson (41 bytes):
    [0xAA][0x55][len=37: u8][rssi: i8][PktHeader: 4][FullStatePayload: 32][crc8]
    CRC-8/MAXIM covers the len byte + all data bytes that follow.

LoRa TIME_SYNC payload — base broadcasts to nodes (12 bytes):
    [PktHeader: 4][TimeSyncPayload: 8]

TIME_SYNC UART frame — Jetson -> base (16 bytes):
    [0xAA][0x55][len=12: u8][PktHeader: 4][TimeSyncPayload: 8][crc8]
    CRC-8/MAXIM covers the len byte + all data bytes.
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
#   session_time(u32) uptime_ms(u32) sensor_flags(u16)
#   wind_cms(u16) temp_cdegc(i16) humidity_cpct(u16)
#   pm1_0_ug10(u16) pm2_5_ug10(u16) pm4_0_ug10(u16) pm10_ug10(u16)
#   lat_e7(i32) lon_e7(i32)
#   →  32 bytes
FULL_STATE_FMT  = "<IIHHhHHHHHii"
FULL_STATE_SIZE = struct.calcsize(FULL_STATE_FMT)  # 32

# TimeSyncPayload: session_id(u32) session_time_ms(u32)  →  8 bytes
TIME_SYNC_PAYLOAD_FMT  = "<II"
TIME_SYNC_PAYLOAD_SIZE = struct.calcsize(TIME_SYNC_PAYLOAD_FMT)  # 8

LORA_PAYLOAD_SIZE   = HEADER_SIZE + FULL_STATE_SIZE         # 36
TIME_SYNC_LORA_SIZE = HEADER_SIZE + TIME_SYNC_PAYLOAD_SIZE  # 12

# Expected len byte values in UART frames
BASE_FRAME_DATA_LEN  = 1 + LORA_PAYLOAD_SIZE    # 37  (rssi + 36)
TIME_SYNC_DATA_LEN   = TIME_SYNC_LORA_SIZE       # 12
TIME_SYNC_FRAME_LEN  = 2 + 1 + TIME_SYNC_DATA_LEN + 1  # 16


# ---------- CRC-8/MAXIM (polynomial 0x31) ----------

def crc8(data: bytes) -> int:
    """Compute CRC-8/MAXIM over data. Matches the C++ implementation in BinaryPacket.h."""
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


# ---------- encode ----------

def encode_time_sync_frame(session_id: int, session_time_ms: int, seq: int = 0) -> bytes:
    """
    Encode a TIME_SYNC UART frame for sending from the Jetson to the base Feather.

    Returns 16 bytes:
        [0xAA][0x55][12][PktHeader(PKT_TIME_SYNC, node_id=0, seq)][TimeSyncPayload][crc8]
    """
    data_len = TIME_SYNC_DATA_LEN  # 12

    hdr = struct.pack(HEADER_FMT, PKT_MAGIC, PKT_TIME_SYNC, 0, seq & 0xFF)
    ts  = struct.pack(TIME_SYNC_PAYLOAD_FMT,
                      session_id & 0xFFFFFFFF,
                      session_time_ms & 0xFFFFFFFF)

    payload = hdr + ts  # 12 bytes

    # CRC covers: len byte + payload bytes
    crc_input = bytes([data_len]) + payload
    frame_crc = crc8(crc_input)

    return bytes([FRAME_M0, FRAME_M1, data_len]) + payload + bytes([frame_crc])


# ---------- decode ----------

def decode_full_state(raw_lora_payload: bytes, rssi: Optional[int] = None) -> Optional[dict]:
    """
    Decode a raw LoRa payload (PktHeader + FullStatePayload) into a dict.

    Parameters
    ----------
    raw_lora_payload : bytes
        Exactly LORA_PAYLOAD_SIZE (36) bytes as received from the radio.
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
        session_time, uptime_ms, sensor_flags,
        wind_cms, temp_cdegc, humidity_cpct,
        pm1_0_ug10, pm2_5_ug10, pm4_0_ug10, pm10_ug10,
        lat_e7, lon_e7,
    ) = struct.unpack_from(FULL_STATE_FMT, raw_lora_payload, HEADER_SIZE)

    return {
        "node_id":         node_id,
        "seq":             seq,
        "session_time_ms": session_time,
        "uptime_ms":       uptime_ms,
        "sensor_flags":    sensor_flags,
        "wind_mps":        round(wind_cms / 100.0, 2),
        "temp_c":          round(temp_cdegc / 100.0, 2),
        "humidity_pct":    round(humidity_cpct / 100.0, 2),
        "pm1_0_ug_m3":     round(pm1_0_ug10 / 10.0, 1),
        "pm2_5_ug_m3":     round(pm2_5_ug10 / 10.0, 1),
        "pm4_0_ug_m3":     round(pm4_0_ug10 / 10.0, 1),
        "pm10_ug_m3":      round(pm10_ug10  / 10.0, 1),
        "lat":             round(lat_e7 / 1e7, 7),
        "lon":             round(lon_e7 / 1e7, 7),
        "rssi":            rssi,
    }
