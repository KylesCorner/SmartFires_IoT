#pragma once

// BinaryPacket.h
//
// Binary wire format for SmartFires telemetry.
//
// UART frame — drone -> feather node:
//   [0xAA][0x55][len=36: u8][PktHeader:4][FullStatePayload:32][crc8]
//   total frame = 40 bytes
//   CRC-8/MAXIM covers the len byte + all data bytes.
//
// LoRa payload — feather node -> feather base:
//   [PktHeader:4][FullStatePayload:32]  = 36 bytes
//   No extra framing; RadioHead handles framing on the radio link.
//
// UART frame — feather base -> Jetson:
//   [0xAA][0x55][len=37: u8][rssi:i8][PktHeader:4][FullStatePayload:32][crc8]
//   total frame = 41 bytes
//   CRC-8/MAXIM covers the len byte + all data bytes (rssi included).
//
// LoRa TIME_SYNC payload — base broadcasts to all nodes:
//   [PktHeader:4][TimeSyncPayload:8]  = 12 bytes
//
// UART TIME_SYNC frame — Jetson -> base, or node feather -> ESP32:
//   [0xAA][0x55][len=12: u8][PktHeader:4][TimeSyncPayload:8][crc8]
//   total frame = 16 bytes

#include <stdint.h>
#include <string.h>

namespace BinaryPacket {

// ---------- frame constants ----------

static constexpr uint8_t FRAME_M0  = 0xAA;
static constexpr uint8_t FRAME_M1  = 0x55;
static constexpr uint8_t PKT_MAGIC = 0xA5;

// ---------- packet types ----------

enum PktType : uint8_t {
    PKT_FULL_STATE = 0x01,
    PKT_HEARTBEAT  = 0x02,
    PKT_TIME_SYNC  = 0x03,  // base station -> nodes (broadcast)
};

// ---------- wire structs (packed — no padding) ----------

struct __attribute__((packed)) PktHeader {
    uint8_t magic;     // PKT_MAGIC = 0xA5
    uint8_t pkt_type;  // PktType enum
    uint8_t node_id;   // compile-time constant per node
    uint8_t seq;       // rolling 0-255; wraps every 256 packets
};

struct __attribute__((packed)) FullStatePayload {
    uint32_t session_time;    // synced ms since session start (Jetson uptime)
    uint32_t uptime_ms;       // local ESP32 millis()
    uint16_t sensor_flags;    // bitmask: WIND=0x01 SHT31=0x02 GPS=0x04 IMU=0x08 SPS30=0x10
    uint16_t wind_cms;        // cm/s  (windMps * 100)
    int16_t  temp_cdegc;      // centi-degrees C  (tempC * 100)
    uint16_t humidity_cpct;   // centi-percent  (humidityPct * 100)
    uint16_t pm1_0_ug10;      // µg/m³ * 10
    uint16_t pm2_5_ug10;      // µg/m³ * 10
    uint16_t pm4_0_ug10;      // µg/m³ * 10
    uint16_t pm10_ug10;       // µg/m³ * 10
    int32_t  lat_e7;          // degrees * 1e7
    int32_t  lon_e7;          // degrees * 1e7
};

struct __attribute__((packed)) TimeSyncPayload {
    uint32_t session_id;      // changes when Jetson restarts; node resets offset on change
    uint32_t session_time_ms; // ms since session start on Jetson
};

// Compile-time size checks
static_assert(sizeof(PktHeader)        ==  4, "PktHeader must be 4 bytes");
static_assert(sizeof(FullStatePayload) == 32, "FullStatePayload must be 32 bytes");
static_assert(sizeof(TimeSyncPayload)  ==  8, "TimeSyncPayload must be 8 bytes");

static constexpr size_t kLoRaPayloadSize =
    sizeof(PktHeader) + sizeof(FullStatePayload);  // 36

static constexpr size_t kTimeSyncLoRaSize =
    sizeof(PktHeader) + sizeof(TimeSyncPayload);   // 12

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
// Writes: [0xAA][0x55][36][PktHeader][FullStatePayload][crc8]  = 40 bytes
// Returns bytes written, or 0 if buf_size is too small (needs >= 40).

inline size_t encodeFullStateFrame(
    uint8_t node_id, uint8_t seq,
    const FullStatePayload& payload,
    uint8_t* buf, size_t buf_size)
{
    static constexpr size_t kDataLen  = sizeof(PktHeader) + sizeof(FullStatePayload); // 36
    static constexpr size_t kFrameLen = 2 + 1 + kDataLen + 1;                         // 40

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

    buf[3 + kDataLen] = crc8(buf + 2, 1 + kDataLen);

    return kFrameLen;
}

// ---------- encode: feather base -> Jetson UART frame ----------
//
// Writes: [0xAA][0x55][37][rssi_i8][PktHeader][FullStatePayload][crc8]  = 41 bytes
// raw_lora_payload must point to kLoRaPayloadSize (36) bytes as received from LoRa.
// Returns bytes written, or 0 if buf_size is too small (needs >= 41).

inline size_t encodeBaseFrame(
    int8_t rssi,
    const uint8_t* raw_lora_payload,
    uint8_t* buf, size_t buf_size)
{
    static constexpr size_t kDataLen  = 1 + kLoRaPayloadSize;  // 37  (rssi + 36)
    static constexpr size_t kFrameLen = 2 + 1 + kDataLen + 1;  // 41

    if (buf_size < kFrameLen) return 0;

    buf[0] = FRAME_M0;
    buf[1] = FRAME_M1;
    buf[2] = static_cast<uint8_t>(kDataLen);
    buf[3] = static_cast<uint8_t>(rssi);

    memcpy(buf + 4, raw_lora_payload, kLoRaPayloadSize);

    buf[4 + kLoRaPayloadSize] = crc8(buf + 2, 1 + kDataLen);

    return kFrameLen;
}

// ---------- encode: TIME_SYNC UART frame (Jetson->base or node->ESP32) ----------
//
// Writes: [0xAA][0x55][12][PktHeader(PKT_TIME_SYNC)][TimeSyncPayload][crc8]  = 16 bytes
// Returns bytes written, or 0 if buf_size is too small (needs >= 16).

inline size_t encodeTimeSyncFrame(
    uint8_t node_id, uint8_t seq,
    const TimeSyncPayload& ts,
    uint8_t* buf, size_t buf_size)
{
    static constexpr size_t kDataLen  = sizeof(PktHeader) + sizeof(TimeSyncPayload); // 12
    static constexpr size_t kFrameLen = 2 + 1 + kDataLen + 1;                        // 16

    if (buf_size < kFrameLen) return 0;

    buf[0] = FRAME_M0;
    buf[1] = FRAME_M1;
    buf[2] = static_cast<uint8_t>(kDataLen);

    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_TIME_SYNC;
    hdr.node_id  = node_id;
    hdr.seq      = seq;

    memcpy(buf + 3,                     &hdr, sizeof(PktHeader));
    memcpy(buf + 3 + sizeof(PktHeader), &ts,  sizeof(TimeSyncPayload));

    buf[3 + kDataLen] = crc8(buf + 2, 1 + kDataLen);

    return kFrameLen;
}

// ---------- decode: validate a raw LoRa FULL_STATE payload ----------
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

// ---------- decode: validate a raw LoRa or UART TIME_SYNC payload ----------
//
// raw_payload must be at least kTimeSyncLoRaSize bytes (the data portion, no framing).
// Returns true if magic and packet type are valid.

inline bool decodeTimeSync(
    const uint8_t* raw_payload, size_t len,
    PktHeader& hdr_out, TimeSyncPayload& ts_out)
{
    if (len < kTimeSyncLoRaSize) return false;

    memcpy(&hdr_out, raw_payload,                     sizeof(PktHeader));
    memcpy(&ts_out,  raw_payload + sizeof(PktHeader), sizeof(TimeSyncPayload));

    return (hdr_out.magic    == PKT_MAGIC)
        && (hdr_out.pkt_type == PKT_TIME_SYNC);
}

} // namespace BinaryPacket
