#pragma once

// BinaryPacket.h
//
// Binary wire format for SmartFires telemetry.
//
// UART frame — drone -> feather node (FULL_STATE):
//   [0xAA][0x55][len=28: u8][PktHeader:4][FullStatePayload:24][crc8]
//   total frame = 32 bytes
//   CRC-8/MAXIM covers the len byte + all data bytes.
//
// UART frame — drone -> feather node (GPS):
//   [0xAA][0x55][len=12: u8][PktHeader:4][GpsPayload:8][crc8]
//   total frame = 16 bytes. Sent once per session when a valid GPS fix is acquired.
//
// UART frame — drone -> feather node (BUNDLE):
//   [0xAA][0x55][len: u8][PktHeader:4][FullStatePayload:24][n_deltas:1][DeltaPayload×n:n*16][crc8]
//   len = 29 + n*16 (max n=7 → len=141, frame=145 bytes)
//
// LoRa payload — feather node -> feather base:
//   FULL_STATE: [PktHeader:4][FullStatePayload:24]  = 28 bytes
//   GPS:        [PktHeader:4][GpsPayload:8]          = 12 bytes. One per session.
//   BUNDLE:     [PktHeader:4][FullStatePayload:24][n_deltas:1][DeltaPayload×n]  ≤ 141 bytes
//   No extra framing; RadioHead handles framing on the radio link.
//
// UART frame — feather base -> Jetson (variable length):
//   [0xAA][0x55][len: u8][rssi:i8][LoRa payload][crc8]
//   len = 1 + lora_payload_len (min 13 for GPS, 29 for FULL_STATE, max 142 for BUNDLE with 7 deltas)
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
    PKT_BUNDLE     = 0x04,  // reference FullStatePayload + N DeltaPayloads
    PKT_GPS        = 0x05,  // one-time GPS fix per session (lat/lon not in regular packets)
};

// ---------- wire structs (packed — no padding) ----------

struct __attribute__((packed)) PktHeader {
    uint8_t magic;     // PKT_MAGIC = 0xA5
    uint8_t pkt_type;  // PktType enum
    uint8_t node_id;   // compile-time constant per node
    uint8_t seq;       // rolling 0-255; wraps every 256 packets
};

// lat/lon removed — transmitted once per session via PKT_GPS to save 8 bytes per packet.
// GPS sensor continues sampling normally on the ESP32; only the wire encoding changed.
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
};

// Sent once per session (PKT_GPS) when the ESP32 first acquires a valid GPS fix.
struct __attribute__((packed)) GpsPayload {
    int32_t lat_e7;   // degrees × 1e7
    int32_t lon_e7;   // degrees × 1e7
};

struct __attribute__((packed)) TimeSyncPayload {
    uint32_t session_id;      // changes when Jetson restarts; node resets offset on change
    uint32_t session_time_ms; // ms since session start on Jetson
};

// DeltaPayload — compact representation of one sensor sample relative to a reference.
//
// wind_cms is stored as an absolute value (not delta) because wind can change rapidly.
// All PM, temperature, and humidity fields are signed deltas from the reference.
// lat/lon removed — drones are stationary; location is in the separate PKT_GPS packet.
// sensor_flags is inherited from the reference frame and not repeated here.
struct __attribute__((packed)) DeltaPayload {
    uint16_t dt_ms;               // ms elapsed since reference session_time
    uint16_t wind_cms;            // absolute wind speed in cm/s
    int16_t  temp_delta_cdegc;    // delta from reference in centi-°C
    int16_t  humidity_delta_cpct; // delta from reference in centi-%
    int16_t  pm1_0_delta;         // delta in µg/m³ × 10
    int16_t  pm2_5_delta;
    int16_t  pm4_0_delta;
    int16_t  pm10_delta;
};

// Compile-time size checks
static_assert(sizeof(PktHeader)        ==  4, "PktHeader must be 4 bytes");
static_assert(sizeof(FullStatePayload) == 24, "FullStatePayload must be 24 bytes");
static_assert(sizeof(GpsPayload)       ==  8, "GpsPayload must be 8 bytes");
static_assert(sizeof(TimeSyncPayload)  ==  8, "TimeSyncPayload must be 8 bytes");
static_assert(sizeof(DeltaPayload)     == 16, "DeltaPayload must be 16 bytes");

static constexpr uint8_t kBundleMaxDeltas = 7;

static constexpr size_t kLoRaPayloadSize =
    sizeof(PktHeader) + sizeof(FullStatePayload);  // 28

static constexpr size_t kGpsLoRaSize =
    sizeof(PktHeader) + sizeof(GpsPayload);        // 12

static constexpr size_t kMaxLoRaPayloadSize =
    sizeof(PktHeader) + sizeof(FullStatePayload) + 1 +
    kBundleMaxDeltas * sizeof(DeltaPayload);  // 141

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

// ---------- encode: drone -> feather node UART frame (FULL_STATE) ----------
//
// Writes: [0xAA][0x55][28][PktHeader][FullStatePayload][crc8]  = 32 bytes
// Returns bytes written, or 0 if buf_size is too small (needs >= 32).

inline size_t encodeFullStateFrame(
    uint8_t node_id, uint8_t seq,
    const FullStatePayload& payload,
    uint8_t* buf, size_t buf_size)
{
    static constexpr size_t kDataLen  = sizeof(PktHeader) + sizeof(FullStatePayload); // 28
    static constexpr size_t kFrameLen = 2 + 1 + kDataLen + 1;                         // 32

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

// ---------- encode: drone -> feather node UART frame (GPS) ----------
//
// Writes: [0xAA][0x55][12][PktHeader(PKT_GPS)][GpsPayload][crc8]  = 16 bytes
// Sent once per session when a valid GPS fix is first acquired.
// The Feather enqueues this for LoRa TX but does NOT ACK it to the ESP32.
// Returns bytes written, or 0 if buf_size is too small (needs >= 16).

inline size_t encodeGpsFrame(
    uint8_t node_id, uint8_t seq,
    const GpsPayload& gps,
    uint8_t* buf, size_t buf_size)
{
    static constexpr size_t kDataLen  = sizeof(PktHeader) + sizeof(GpsPayload); // 12
    static constexpr size_t kFrameLen = 2 + 1 + kDataLen + 1;                   // 16

    if (buf_size < kFrameLen) return 0;

    buf[0] = FRAME_M0;
    buf[1] = FRAME_M1;
    buf[2] = static_cast<uint8_t>(kDataLen);

    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_GPS;
    hdr.node_id  = node_id;
    hdr.seq      = seq;

    memcpy(buf + 3,                     &hdr, sizeof(PktHeader));
    memcpy(buf + 3 + sizeof(PktHeader), &gps, sizeof(GpsPayload));

    buf[3 + kDataLen] = crc8(buf + 2, 1 + kDataLen);

    return kFrameLen;
}

// ---------- encode: feather base -> Jetson UART frame (variable length) ----------
//
// Writes: [0xAA][0x55][len][rssi_i8][LoRa payload][crc8]
//   len = 1 + lora_len  (rssi byte + LoRa payload bytes)
//   GPS:        lora_len=12 → total frame = 17 bytes
//   FULL_STATE: lora_len=28 → total frame = 33 bytes
//   BUNDLE:     lora_len up to 141 → total frame up to 146 bytes
// raw_lora_payload must point to lora_len bytes as received from LoRa.
// Returns bytes written, or 0 on failure.

inline size_t encodeBaseFrame(
    int8_t rssi,
    const uint8_t* raw_lora_payload, size_t lora_len,
    uint8_t* buf, size_t buf_size)
{
    const size_t kDataLen  = 1 + lora_len;
    const size_t kFrameLen = 2 + 1 + kDataLen + 1;

    if (kDataLen > 255)        return 0;  // len byte is uint8_t
    if (buf_size < kFrameLen)  return 0;

    buf[0] = FRAME_M0;
    buf[1] = FRAME_M1;
    buf[2] = static_cast<uint8_t>(kDataLen);
    buf[3] = static_cast<uint8_t>(rssi);

    memcpy(buf + 4, raw_lora_payload, lora_len);

    buf[4 + lora_len] = crc8(buf + 2, 1 + kDataLen);

    return kFrameLen;
}

// ---------- encode: drone -> feather node UART bundle frame ----------
//
// Writes: [0xAA][0x55][len][PktHeader][FullStatePayload][n_deltas][DeltaPayload×n][crc8]
//   len = sizeof(PktHeader) + sizeof(FullStatePayload) + 1 + n_deltas * sizeof(DeltaPayload)
//   Max n_deltas = kBundleMaxDeltas (7) → len=141, frame=145 bytes
// Returns bytes written, or 0 on failure.

inline size_t encodeBundleFrame(
    uint8_t node_id, uint8_t seq,
    const FullStatePayload& ref,
    const DeltaPayload* deltas, uint8_t n_deltas,
    uint8_t* buf, size_t buf_size)
{
    if (n_deltas > kBundleMaxDeltas) return 0;

    const size_t kDataLen  = sizeof(PktHeader) + sizeof(FullStatePayload)
                           + 1 + static_cast<size_t>(n_deltas) * sizeof(DeltaPayload);
    const size_t kFrameLen = 2 + 1 + kDataLen + 1;

    if (kDataLen > 255)        return 0;
    if (buf_size < kFrameLen)  return 0;

    buf[0] = FRAME_M0;
    buf[1] = FRAME_M1;
    buf[2] = static_cast<uint8_t>(kDataLen);

    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_BUNDLE;
    hdr.node_id  = node_id;
    hdr.seq      = seq;

    size_t off = 3;
    memcpy(buf + off, &hdr, sizeof(PktHeader));        off += sizeof(PktHeader);
    memcpy(buf + off, &ref, sizeof(FullStatePayload)); off += sizeof(FullStatePayload);
    buf[off++] = n_deltas;
    for (uint8_t i = 0; i < n_deltas; ++i) {
        memcpy(buf + off, &deltas[i], sizeof(DeltaPayload));
        off += sizeof(DeltaPayload);
    }

    buf[off] = crc8(buf + 2, 1 + kDataLen);
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

// ---------- decode: validate a raw LoRa GPS payload ----------
//
// raw_payload must be at least kGpsLoRaSize bytes.
// Returns true if magic and packet type are valid.

inline bool decodeGps(
    const uint8_t* raw_payload, size_t len,
    PktHeader& hdr_out, GpsPayload& gps_out)
{
    if (len < kGpsLoRaSize) return false;

    memcpy(&hdr_out, raw_payload,                     sizeof(PktHeader));
    memcpy(&gps_out, raw_payload + sizeof(PktHeader), sizeof(GpsPayload));

    return (hdr_out.magic    == PKT_MAGIC)
        && (hdr_out.pkt_type == PKT_GPS);
}

// ---------- decode: validate a raw LoRa BUNDLE payload ----------
//
// raw_payload: pointer to the LoRa payload bytes (no UART framing).
// deltas_out must point to a buffer of at least kBundleMaxDeltas DeltaPayload entries.
// Returns true if the bundle is valid; delta_count_out holds the number of deltas decoded.

inline bool decodeBundle(
    const uint8_t* raw_payload, size_t len,
    PktHeader& hdr_out, FullStatePayload& ref_out,
    uint8_t& delta_count_out, DeltaPayload* deltas_out)
{
    const size_t kMinLen = sizeof(PktHeader) + sizeof(FullStatePayload) + 1;
    if (len < kMinLen) return false;

    size_t off = 0;
    memcpy(&hdr_out, raw_payload + off, sizeof(PktHeader)); off += sizeof(PktHeader);
    if (hdr_out.magic != PKT_MAGIC || hdr_out.pkt_type != PKT_BUNDLE) return false;

    memcpy(&ref_out, raw_payload + off, sizeof(FullStatePayload)); off += sizeof(FullStatePayload);
    delta_count_out = raw_payload[off++];

    if (delta_count_out > kBundleMaxDeltas) return false;
    if (len < off + static_cast<size_t>(delta_count_out) * sizeof(DeltaPayload)) return false;

    for (uint8_t i = 0; i < delta_count_out; ++i) {
        memcpy(&deltas_out[i], raw_payload + off, sizeof(DeltaPayload));
        off += sizeof(DeltaPayload);
    }
    return true;
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
