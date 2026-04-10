#pragma once

// BinaryPacket.h
//
// Binary wire format for SmartFires telemetry.
//
// UART frame — drone -> feather node:
//   [0xAA][0x55][len:u8][PktHeader:4][FullStatePayload:27][crc8]
//   len = 31  |  total frame = 35 bytes
//   CRC-8/MAXIM covers the len byte + all data bytes.
//
// LoRa payload — feather node -> feather base:
//   [PktHeader:4][FullStatePayload:27]  = 31 bytes
//   No extra framing; RadioHead handles framing on the radio link.
//
// UART frame — feather base -> Jetson:
//   [0xAA][0x55][len:u8][rssi:i8][PktHeader:4][FullStatePayload:27][crc8]
//   len = 32  |  total frame = 36 bytes
//   CRC-8/MAXIM covers the len byte + all data bytes (rssi included).

#include <stdint.h>
#include <string.h>

namespace BinaryPacket {

// ---------- frame constants ----------

static constexpr uint8_t FRAME_M0  = 0xAA;
static constexpr uint8_t FRAME_M1  = 0x55;
static constexpr uint8_t PKT_MAGIC = 0xA5;  // first byte of PktHeader

// ---------- packet types ----------

enum PktType : uint8_t {
    PKT_FULL_STATE = 0x01,
    PKT_HEARTBEAT  = 0x02,
    PKT_TIME_SYNC  = 0x03,  // Phase 2: base station -> nodes
};

// ---------- wire structs (packed — no padding) ----------

struct __attribute__((packed)) PktHeader {
    uint8_t magic;     // PKT_MAGIC = 0xA5
    uint8_t pkt_type;  // PktType enum
    uint8_t node_id;   // compile-time constant per node
    uint8_t seq;       // rolling 0-255; wraps every 256 packets
};

struct __attribute__((packed)) FullStatePayload {
    uint32_t session_time;   // ms — local millis() until TIME_SYNC is live
    uint32_t uptime_ms;
    uint16_t sensor_flags;   // same bitmask as TelemetryFlags in drone/main.cpp
    uint8_t  flame;          // 0 or 1
    uint16_t wind_cms;       // cm/s  (windMps * 100)
    int16_t  temp_cdegc;     // centi-degrees C  (tempC * 100)
    uint16_t humidity_cpct;  // centi-percent  (humidityPct * 100)
    uint16_t lidar_cm;
    int32_t  lat_e7;         // degrees * 1e7
    int32_t  lon_e7;         // degrees * 1e7
};

// Compile-time size checks — caught at build time if packing is wrong
static_assert(sizeof(PktHeader)        ==  4, "PktHeader must be 4 bytes");
static_assert(sizeof(FullStatePayload) == 27, "FullStatePayload must be 27 bytes");

static constexpr size_t kLoRaPayloadSize =
    sizeof(PktHeader) + sizeof(FullStatePayload);  // 31

// ---------- CRC-8/MAXIM (polynomial 0x31) ----------

inline uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                               : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

// ---------- encode: drone -> feather node UART frame ----------
//
// Writes: [0xAA][0x55][31][PktHeader][FullStatePayload][crc8]
// Returns bytes written, or 0 if buf_size is too small (needs >= 35).

inline size_t encodeFullStateFrame(
    uint8_t node_id, uint8_t seq,
    const FullStatePayload& payload,
    uint8_t* buf, size_t buf_size)
{
    static constexpr size_t kDataLen  = sizeof(PktHeader) + sizeof(FullStatePayload); // 31
    static constexpr size_t kFrameLen = 2 + 1 + kDataLen + 1; // 35

    if (buf_size < kFrameLen) return 0;

    buf[0] = FRAME_M0;
    buf[1] = FRAME_M1;
    buf[2] = static_cast<uint8_t>(kDataLen);

    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_FULL_STATE;
    hdr.node_id  = node_id;
    hdr.seq      = seq;

    memcpy(buf + 3,                     &hdr,     sizeof(PktHeader));
    memcpy(buf + 3 + sizeof(PktHeader), &payload, sizeof(FullStatePayload));

    // CRC covers: len byte + data bytes
    buf[3 + kDataLen] = crc8(buf + 2, 1 + kDataLen);

    return kFrameLen;
}

// ---------- encode: feather base -> Jetson UART frame ----------
//
// Writes: [0xAA][0x55][32][rssi_i8][PktHeader][FullStatePayload][crc8]
// raw_lora_payload must point to kLoRaPayloadSize (31) bytes as received from LoRa.
// Returns bytes written, or 0 if buf_size is too small (needs >= 36).

inline size_t encodeBaseFrame(
    int8_t rssi,
    const uint8_t* raw_lora_payload,
    uint8_t* buf, size_t buf_size)
{
    static constexpr size_t kDataLen  = 1 + kLoRaPayloadSize; // 32 (rssi + 31)
    static constexpr size_t kFrameLen = 2 + 1 + kDataLen + 1; // 36

    if (buf_size < kFrameLen) return 0;

    buf[0] = FRAME_M0;
    buf[1] = FRAME_M1;
    buf[2] = static_cast<uint8_t>(kDataLen);
    buf[3] = static_cast<uint8_t>(rssi);

    memcpy(buf + 4, raw_lora_payload, kLoRaPayloadSize);

    // CRC covers: len byte + data bytes (rssi + lora payload)
    buf[4 + kLoRaPayloadSize] = crc8(buf + 2, 1 + kDataLen);

    return kFrameLen;
}

// ---------- decode: validate a raw LoRa payload ----------
//
// raw_payload must be at least kLoRaPayloadSize bytes.
// Returns true if magic and packet type are valid.

inline bool decodeFullState(
    const uint8_t* raw_payload, size_t len,
    PktHeader& hdr_out, FullStatePayload& payload_out)
{
    if (len < kLoRaPayloadSize) return false;

    memcpy(&hdr_out,     raw_payload,                     sizeof(PktHeader));
    memcpy(&payload_out, raw_payload + sizeof(PktHeader), sizeof(FullStatePayload));

    return (hdr_out.magic    == PKT_MAGIC)
        && (hdr_out.pkt_type == PKT_FULL_STATE);
}

} // namespace BinaryPacket
