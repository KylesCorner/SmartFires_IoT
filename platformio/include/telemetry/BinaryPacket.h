#pragma once

// Binary wire format for SmartFires telemetry.
//
// LoRa payloads — node -> base:
//   AWAKEN:     [PktHeader:4]                                                    =  4 bytes
//   BUNDLE:     [PktHeader:4][FullStatePayload:20][n_deltas:1][DeltaPayload×n]  ≤ 137 bytes
//   STATUS:     [PktHeader:4][StatusPayload:12]                                  = 16 bytes
//
// LoRa TIME_SYNC — base -> all nodes (broadcast):
//   TIME_SYNC:  [PktHeader:4][TimeSyncPayload:8]                                 = 12 bytes
//
// UART TIME_SYNC frame — Jetson -> base (16 bytes):
//   [0xAA][0x55][len=12][PktHeader(PKT_TIME_SYNC):4][TimeSyncPayload:8][crc8]
//
// UART base frame — base -> Jetson (variable):
//   [0xAA][0x55][len:u8][rssi:i8][LoRa payload][crc8]
//
// CRC: CRC-8/MAXIM (polynomial 0x31), covers len byte + all data bytes.

#include <stdint.h>
#include <string.h>

namespace BinaryPacket {

static constexpr uint8_t FRAME_M0  = 0xAA;
static constexpr uint8_t FRAME_M1  = 0x55;
static constexpr uint8_t PKT_MAGIC = 0xA5;

enum PktType : uint8_t {
    PKT_FULL_STATE = 0x01,
    PKT_HEARTBEAT  = 0x02,
    PKT_TIME_SYNC  = 0x03,
    PKT_BUNDLE     = 0x04,
    PKT_STATUS     = 0x05,  // GPS + battery, sent every 15 min
    PKT_AWAKEN     = 0x06,  // boot handshake — node broadcasts before sensing starts
};

struct __attribute__((packed)) PktHeader {
    uint8_t magic;
    uint8_t pkt_type;
    uint8_t node_id;
    uint8_t seq;
};

struct __attribute__((packed)) FullStatePayload {
    uint32_t session_time;      // synced ms since Jetson session start
    uint16_t sensor_flags;      // WIND=0x01 SHT31=0x02 GPS=0x04 IMU=0x08 SPS30=0x10
    uint16_t wind_cms;          // cm/s
    int16_t  temp_cdegc;        // centi-°C
    uint16_t humidity_cpct;     // centi-%
    uint16_t pm1_0_ug10;        // µg/m³ × 10
    uint16_t pm2_5_ug10;
    uint16_t pm4_0_ug10;
    uint16_t pm10_ug10;
};

// Sent every 15 minutes — GPS position + battery level.
// flags bits: STATUS_GPS_VALID=0x01, STATUS_BATT_VALID=0x02
static constexpr uint8_t STATUS_GPS_VALID  = 0x01;
static constexpr uint8_t STATUS_BATT_VALID = 0x02;

struct __attribute__((packed)) StatusPayload {
    int32_t  lat_e7;        // degrees × 1e7  (valid if STATUS_GPS_VALID)
    int32_t  lon_e7;        // degrees × 1e7  (valid if STATUS_GPS_VALID)
    uint16_t battery_mv;    // millivolts      (valid if STATUS_BATT_VALID)
    uint8_t  battery_pct;   // 0–100           (valid if STATUS_BATT_VALID)
    uint8_t  flags;         // STATUS_GPS_VALID | STATUS_BATT_VALID
};

struct __attribute__((packed)) TimeSyncPayload {
    uint32_t session_id;        // random; change triggers STATUS re-send and clock reset
    uint32_t session_time_ms;   // ms since receiver.py started
};

// wind_cms is absolute (not a delta) — wind changes too fast to delta-encode reliably.
// All other fields are signed deltas from the bundle's reference FullStatePayload.
struct __attribute__((packed)) DeltaPayload {
    uint16_t dt_ms;
    uint16_t wind_cms;
    int16_t  temp_delta_cdegc;
    int16_t  humidity_delta_cpct;
    int16_t  pm1_0_delta;
    int16_t  pm2_5_delta;
    int16_t  pm4_0_delta;
    int16_t  pm10_delta;
};

static_assert(sizeof(PktHeader)        ==  4, "PktHeader must be 4 bytes");
static_assert(sizeof(FullStatePayload) == 20, "FullStatePayload must be 20 bytes");
static_assert(sizeof(StatusPayload)    == 12, "StatusPayload must be 12 bytes");
static_assert(sizeof(TimeSyncPayload)  ==  8, "TimeSyncPayload must be 8 bytes");
static_assert(sizeof(DeltaPayload)     == 16, "DeltaPayload must be 16 bytes");

static constexpr uint8_t kBundleMaxDeltas = 7;

// LoRa payload sizes (no UART framing).
static constexpr size_t kAwakenLoRaSize =
    sizeof(PktHeader);                                                  //  4
static constexpr size_t kStatusLoRaSize =
    sizeof(PktHeader) + sizeof(StatusPayload);                          // 16
static constexpr size_t kTimeSyncLoRaSize =
    sizeof(PktHeader) + sizeof(TimeSyncPayload);                        // 12
static constexpr size_t kFullStateLoRaSize =
    sizeof(PktHeader) + sizeof(FullStatePayload);                       // 24
static constexpr size_t kMaxBundleLoRaSize =
    sizeof(PktHeader) + sizeof(FullStatePayload) + 1 +
    kBundleMaxDeltas * sizeof(DeltaPayload);                            // 137

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

// ---------- encode: raw LoRa AWAKEN payload (4 bytes) ----------

inline uint8_t encodeAwakenPayload(
    uint8_t node_id, uint8_t seq,
    uint8_t* buf, size_t buf_size)
{
    if (buf_size < kAwakenLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_AWAKEN;
    hdr.node_id  = node_id;
    hdr.seq      = seq;
    memcpy(buf, &hdr, sizeof(PktHeader));
    return static_cast<uint8_t>(kAwakenLoRaSize);
}

// ---------- encode: raw LoRa STATUS payload (16 bytes) ----------

inline uint8_t encodeStatusPayload(
    uint8_t node_id, uint8_t seq,
    const StatusPayload& sp,
    uint8_t* buf, size_t buf_size)
{
    if (buf_size < kStatusLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_STATUS;
    hdr.node_id  = node_id;
    hdr.seq      = seq;
    memcpy(buf,                    &hdr, sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &sp,  sizeof(StatusPayload));
    return static_cast<uint8_t>(kStatusLoRaSize);
}

// ---------- encode: raw LoRa TIME_SYNC payload (12 bytes, base -> nodes broadcast) ----------

inline uint8_t encodeTimeSyncPayload(
    uint8_t seq,
    const TimeSyncPayload& ts,
    uint8_t* buf, size_t buf_size)
{
    if (buf_size < kTimeSyncLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_TIME_SYNC;
    hdr.node_id  = 0;
    hdr.seq      = seq;
    memcpy(buf,                    &hdr, sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &ts,  sizeof(TimeSyncPayload));
    return static_cast<uint8_t>(kTimeSyncLoRaSize);
}

// ---------- encode: raw LoRa BUNDLE payload ----------
//
// Output: [PktHeader:4][FullStatePayload:20][n_deltas:1][DeltaPayload×n]
// Returns bytes written, or 0 on failure.

inline uint8_t encodeBundlePayload(
    uint8_t node_id, uint8_t seq,
    const FullStatePayload& ref,
    const DeltaPayload* deltas, uint8_t n_deltas,
    uint8_t* buf, size_t buf_size)
{
    if (n_deltas > kBundleMaxDeltas) return 0;

    const size_t len = sizeof(PktHeader) + sizeof(FullStatePayload)
                     + 1 + static_cast<size_t>(n_deltas) * sizeof(DeltaPayload);
    if (buf_size < len) return 0;

    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_BUNDLE;
    hdr.node_id  = node_id;
    hdr.seq      = seq;

    size_t off = 0;
    memcpy(buf + off, &hdr, sizeof(PktHeader));        off += sizeof(PktHeader);
    memcpy(buf + off, &ref, sizeof(FullStatePayload)); off += sizeof(FullStatePayload);
    buf[off++] = n_deltas;
    for (uint8_t i = 0; i < n_deltas; ++i) {
        memcpy(buf + off, &deltas[i], sizeof(DeltaPayload));
        off += sizeof(DeltaPayload);
    }

    return static_cast<uint8_t>(len);
}

// ---------- encode: TIME_SYNC UART frame (Jetson -> base, 16 bytes) ----------

inline size_t encodeTimeSyncFrame(
    uint8_t node_id, uint8_t seq,
    const TimeSyncPayload& ts,
    uint8_t* buf, size_t buf_size)
{
    static constexpr size_t kDataLen  = sizeof(PktHeader) + sizeof(TimeSyncPayload);
    static constexpr size_t kFrameLen = 2 + 1 + kDataLen + 1;

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

// ---------- encode: base -> Jetson UART frame (variable length) ----------

inline size_t encodeBaseFrame(
    int8_t rssi,
    const uint8_t* raw_lora_payload, size_t lora_len,
    uint8_t* buf, size_t buf_size)
{
    const size_t kDataLen  = 1 + lora_len;
    const size_t kFrameLen = 2 + 1 + kDataLen + 1;
    if (kDataLen > 255 || buf_size < kFrameLen) return 0;

    buf[0] = FRAME_M0;
    buf[1] = FRAME_M1;
    buf[2] = static_cast<uint8_t>(kDataLen);
    buf[3] = static_cast<uint8_t>(rssi);
    memcpy(buf + 4, raw_lora_payload, lora_len);
    buf[4 + lora_len] = crc8(buf + 2, 1 + kDataLen);
    return kFrameLen;
}

// ---------- decode ----------

inline bool decodeBundle(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, FullStatePayload& ref_out,
    uint8_t& delta_count_out, DeltaPayload* deltas_out)
{
    const size_t kMin = sizeof(PktHeader) + sizeof(FullStatePayload) + 1;
    if (len < kMin) return false;

    size_t off = 0;
    memcpy(&hdr_out, raw + off, sizeof(PktHeader)); off += sizeof(PktHeader);
    if (hdr_out.magic != PKT_MAGIC || hdr_out.pkt_type != PKT_BUNDLE) return false;

    memcpy(&ref_out, raw + off, sizeof(FullStatePayload)); off += sizeof(FullStatePayload);
    delta_count_out = raw[off++];
    if (delta_count_out > kBundleMaxDeltas) return false;
    if (len < off + static_cast<size_t>(delta_count_out) * sizeof(DeltaPayload)) return false;

    for (uint8_t i = 0; i < delta_count_out; ++i) {
        memcpy(&deltas_out[i], raw + off, sizeof(DeltaPayload));
        off += sizeof(DeltaPayload);
    }
    return true;
}

inline bool decodeStatus(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, StatusPayload& sp_out)
{
    if (len < kStatusLoRaSize) return false;
    memcpy(&hdr_out, raw,                    sizeof(PktHeader));
    memcpy(&sp_out,  raw + sizeof(PktHeader), sizeof(StatusPayload));
    return hdr_out.magic == PKT_MAGIC && hdr_out.pkt_type == PKT_STATUS;
}

inline bool decodeTimeSync(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, TimeSyncPayload& ts_out)
{
    if (len < kTimeSyncLoRaSize) return false;
    memcpy(&hdr_out, raw,                    sizeof(PktHeader));
    memcpy(&ts_out,  raw + sizeof(PktHeader), sizeof(TimeSyncPayload));
    return hdr_out.magic == PKT_MAGIC && hdr_out.pkt_type == PKT_TIME_SYNC;
}

} // namespace BinaryPacket
