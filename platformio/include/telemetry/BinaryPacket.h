// ---
// description: Binary wire-format definitions for SmartFires telemetry — packet structs, CRC-8/MAXIM, and the encode/decode functions for every LoRa and UART frame type.
// role: implementation
// docs: [bandwidth-scaling, jetson-bridge, software-design]
// ---
#pragma once

// Binary wire format for SmartFires telemetry.
//
// LoRa payloads — node -> base:
//   AWAKEN:     [PktHeader:5][AwakenPayload:6][crc8:1]                                    =  12 bytes
//              (legacy pre-flags frame used a 4-byte header + 4-byte payload = 9 bytes;
//               decodeAwaken still accepts it — see decodeAwaken's comment for the caveat)
//   BUNDLE:     [PktHeader:5][FullStatePayload:20][n_deltas:1][DeltaPayload×n][crc8:1]  ≤ 195 bytes
//   STATUS:     [PktHeader:5][StatusPayload:21][crc8:1]                                 =  27 bytes
//   CMD_ACK:    [PktHeader:5][CmdAckPayload:6][crc8:1]                                  =  12 bytes
//   WINDOW_BEGIN/WINDOW_END:
//               [PktHeader:5][WindowMarkerPayload:11][crc8:1]                           =  17 bytes
//
// LoRa TIME_SYNC — base -> node or all nodes:
//   TIME_SYNC:  [PktHeader:5][TimeSyncPayload:8][crc8:1]                                =  14 bytes
//
// LoRa CMD_SET_TX_POWER — base -> one node:
//   CMD_SET_TX_POWER:
//               [PktHeader:5][CmdSetTxPowerPayload:3][crc8:1]                           =   9 bytes
//
// CRC-8/MAXIM (polynomial 0x31) covers all preceding bytes in the LoRa payload.
//
// UART TIME_SYNC frame — Jetson -> base (18 bytes):
//   [0xAA][0x55][len=14][PktHeader(PKT_TIME_SYNC):5][TimeSyncPayload:8]
//   [LoRa crc8][UART crc8]
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
    PKT_STATUS     = 0x05,  // GPS/battery/heading/link/power; cadence is build-configured
    PKT_AWAKEN     = 0x06,  // boot handshake — node broadcasts before sensing starts
    PKT_ACK_SUMMARY = 0x07, // base -> node app-layer reliability summary
    // Timed duty-cycle active-window edges. Sent by the node instead of marking
    // a data frame, so the "I am about to sleep" signal is carried by something
    // that is never retransmitted and never awaits an ack — see
    // WindowMarkerPayload below.
    PKT_WINDOW_BEGIN = 0x08,
    PKT_WINDOW_END   = 0x09,
    PKT_CMD_CALIBRATE    = 0x10,
    PKT_CMD_RESET        = 0x11,
    PKT_CALIBRATION_DATA = 0x12,
    PKT_CMD_ACK          = 0x13,
    // Base -> Jetson only, never sent over LoRa. Payload is PktHeader followed
    // by a raw @SFDBG text line (see logging/DebugLogger.h) — no fixed struct,
    // no embedded crc8, since the outer UART/USB frame's crc8 already covers
    // it end-to-end on this single hop.
    PKT_DEBUG_LOG        = 0x14,
    // Base -> one node. The base station is the sole authority on node TX
    // power (see documentation/Pending_Plans/DYNAMIC_TX_POWER.md): the node
    // never decides its own level, it only applies what it is told, clamps it
    // to the locally-known-safe range, and reports the applied value back in
    // StatusPayload::tx_power_dbm. Acked with the existing PKT_CMD_ACK.
    PKT_CMD_SET_TX_POWER = 0x15,
};

struct __attribute__((packed)) PktHeader {
    uint8_t magic;
    uint8_t pkt_type;
    uint8_t node_id;
    uint8_t seq;
    uint8_t flags;
};

// PktHeader::flags — per-packet bits, present on every packet type.
//
// RETX marks an app-layer retransmission of a telemetry frame the node already
// put on the air (TdmaRadioService::pickRetransmitCandidate stamps it into the
// outgoing copy and recomputes the crc8; the stored pending entry is untouched).
// It tells the base its previous ACK_SUMMARY never landed, and gives the Jetson
// a way to tell replayed samples from first-transmission ones.
//
// 0x01/0x02 were PKT_FLAG_WINDOW_FIRST/PKT_FLAG_WINDOW_LAST, which bounded a
// Timed active window by marking the bundles at its edges. They are retired in
// favour of the PKT_WINDOW_BEGIN/PKT_WINDOW_END frames: gluing "I am about to
// sleep" onto a retransmittable data frame meant a replayed WINDOW_LAST asserted
// the opposite of a fresh one, and it made the last bundle of every window
// structurally unackable — its ack could only be sent in a slot 0 falling after
// standby had begun, so it was re-sent in full on the next wake purely to prompt
// the ack. The bit values stay reserved so a stale node's frames can never be
// misread as carrying some later meaning.
static constexpr uint8_t PKT_FLAG_RETX          = 0x04;
static constexpr uint8_t PKT_FLAG_RESERVED_0x01 = 0x01;  // was WINDOW_FIRST
static constexpr uint8_t PKT_FLAG_RESERVED_0x02 = 0x02;  // was WINDOW_LAST

struct __attribute__((packed)) AwakenPayload {
    uint32_t uid_hash;
    uint8_t  reset_cause;   // raw PM->RCAUSE.reg from this boot (WDT/BOD/POR/...)
    uint8_t  hang_zone;     // HangZone breadcrumb (platform/ResetDiagnostics.h);
                            // 0 = ZONE_UNKNOWN when not a WDT reset or breadcrumb invalid
};

// Carried by PKT_WINDOW_BEGIN and PKT_WINDOW_END — the Timed duty-cycle active
// window's edges.
//
// These frames are deliberately outside the reliability system: they are never
// entered into TdmaRadioService's pending window (isTelemetryPacketForNode()
// allowlists BUNDLE/STATUS/FULL_STATE), never retransmitted, and never consume a
// PktHeader::seq — seq is the ack bitmap's index, and a fire-and-forget frame
// burning one would leave a hole the node can never fill, stalling ackBaseSeq for
// 16 sequences and inflating the Jetson's loss stats. window_id is their own
// counter instead.
//
// Losing one degrades gracefully: a lost WINDOW_END costs the base some slot-0
// airtime acking a node that is already asleep, and a lost WINDOW_BEGIN costs one
// retransmission to prompt the deferred ack — which is exactly what the retired
// WINDOW_LAST flag did on *every* window.
//
// session_time_ms is the window edge's own instant, which no other frame carries:
// the last bundle's final sample is taken before the window closes, not at the
// close. Timestamping the markers lets the receiver attribute bundles to windows
// by time rather than by arrival order, which survives loss and reordering.
struct __attribute__((packed)) WindowMarkerPayload {
    uint32_t session_time_ms;   // session clock at the window edge
    uint32_t planned_sleep_ms;  // WINDOW_END: standby about to be entered; 0 on BEGIN
    uint16_t window_id;         // per-node window counter, wraps at 65535
    uint8_t  sample_count;      // WINDOW_END: samples in the window just closed; 0 on BEGIN
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

// GPS position, battery level, DMP heading, link totals, and applied TX power.
// Cadence is supplied by NetworkConfig; active node environments use 15 s.
// flags bits: STATUS_GPS_VALID=0x01, STATUS_BATT_VALID=0x02, STATUS_IMU_VALID=0x04
static constexpr uint8_t STATUS_GPS_VALID  = 0x01;
static constexpr uint8_t STATUS_BATT_VALID = 0x02;
static constexpr uint8_t STATUS_IMU_VALID  = 0x04;
// Set when the node's TX power is pinned by an operator (TX_POWER_MODE_STATIC);
// clear means the base's control loop owns it. Rides in the existing flags byte
// rather than costing StatusPayload another byte — unlike the three bits above
// it is a mode, not a validity flag, but the byte had room and a 1-byte frame
// growth to carry one bit is not worth it.
static constexpr uint8_t STATUS_TX_POWER_STATIC = 0x08;

struct __attribute__((packed)) StatusPayload {
    int32_t  lat_e7;            // degrees × 1e7  (valid if STATUS_GPS_VALID)
    int32_t  lon_e7;            // degrees × 1e7  (valid if STATUS_GPS_VALID)
    uint16_t battery_mv;        // millivolts      (valid if STATUS_BATT_VALID)
    uint8_t  battery_pct;       // 0–100           (valid if STATUS_BATT_VALID)
    uint8_t  flags;             // STATUS_GPS_VALID | STATUS_BATT_VALID | STATUS_IMU_VALID
    uint16_t heading_deg_x10;   // heading × 10, 0–3590  (valid if STATUS_IMU_VALID)
    uint16_t heading_accuracy;  // Q12 raw; divide by 4096 for degrees
    uint16_t retx_total;        // lifetime retransmit count, saturated at 65535
    uint16_t fail_total;        // lifetime send-failure count, saturated at 65535
    // TX power the node has actually applied to its radio, in dBm. Always
    // populated (no validity flag): the node knows this unconditionally, it is
    // not a sensor reading that can fail.
    //
    // Deliberately sourced from the node rather than from the base's own record
    // of what it last commanded — a CMD_SET_TX_POWER whose CMD_ACK was lost, or
    // one that arrived and was clamped, leaves the two disagreeing, and this
    // field is the side that reflects what is actually on the air. Signed so a
    // future radio with a negative-dBm range needs no format change.
    int8_t   tx_power_dbm;
};

struct __attribute__((packed)) CmdCalibratePayload {
    uint8_t node_id;
    uint8_t duration_s;
};

struct __attribute__((packed)) CmdResetPayload {
    uint8_t node_id;
    uint8_t reset_type;
};

// Per-node TX power control mode.
//
// DYNAMIC: the base's control loop owns this node's power and may step it.
// STATIC:  an operator has pinned the level; the base's loop leaves it alone.
//
// The mode is enforced on the base (it is the only thing that decides) but is
// also held and reported by the node, so the dashboard shows what the node is
// really in rather than what the base believes it commanded — same ground-truth
// argument as StatusPayload::tx_power_dbm.
//
// STATIC is an operator override, not a safety state: a node that loses contact
// reverts to DYNAMIC at baseline along with everything else (see
// TdmaClock::syncStale handling in SmartFiresNodeApp). There is exactly one
// fallback rule, and pinning a level does not exempt a node from it.
static constexpr uint8_t TX_POWER_MODE_DYNAMIC = 0x00;
static constexpr uint8_t TX_POWER_MODE_STATIC  = 0x01;

// Carried by PKT_CMD_SET_TX_POWER. node_id is redundant with PktHeader::node_id
// exactly as it is on CmdCalibratePayload/CmdResetPayload — the header field is
// 0 on base-originated command frames, so the target lives in the payload.
//
// No seq field: PktHeader::seq already sequences these, the same as it does for
// CALIBRATE/RESET, and a second copy could only ever disagree with it.
//
// tx_power_dbm is always ABSOLUTE, never a delta — deliberately. The base does
// not need to know a node's current level to command it safely, only to decide
// whether to command at all, so any desync (base reboot, lost CMD_ACK, a node
// reset the base never saw) is always recoverable by sending an absolute
// baseline. A relative "step by N" variant would break that: a base that
// rebooted believing a node was at 13 dBm when it was really at 7 would issue
// "step down" and drive it up. Do not add one.
struct __attribute__((packed)) CmdSetTxPowerPayload {
    uint8_t node_id;
    int8_t  tx_power_dbm;   // requested power; the node clamps before applying
    uint8_t mode;           // TX_POWER_MODE_DYNAMIC | TX_POWER_MODE_STATIC
};

struct __attribute__((packed)) CmdAckPayload {
    uint8_t  cmd_type;
    uint32_t uid_hash;
    uint8_t  status;
};

struct __attribute__((packed)) TimeSyncPayload {
    uint32_t session_id;        // random; change triggers STATUS re-send and clock reset
    uint32_t session_time_ms;   // ms since receiver.py started
};

// Base station summary for one node's recently received sequence numbers.
// ack_base_seq acknowledges all contiguous packets up to and including base.
// ack_mask bit i acknowledges sequence (ack_base_seq + 1 + i), i in [0..15].
struct __attribute__((packed)) AckSummaryPayload {
    uint8_t  node_id;
    uint8_t  ack_base_seq;
    uint16_t ack_mask;
};

// wind_cms is absolute (not a delta) — wind changes too fast to delta-encode reliably.
// Compact delta encoding (12 bytes) keeps PM2.5/PM10 high precision while coarsening
// slower channels to improve payload efficiency.
struct __attribute__((packed)) DeltaPayload {
    uint8_t  dt_ticks_250ms;        // dt in 250 ms ticks from reference
    uint16_t wind_cms;
    int8_t   temp_delta_deci_c;     // 0.1 C units (from cdegC / 10)
    int8_t   humidity_delta_0p2pct; // 0.2 %RH units (from cpct / 20)
    int8_t   pm1_0_delta_ug;        // 1.0 ug/m3 units (from ug10 / 10)
    int16_t  pm2_5_delta_ug10;      // 0.1 ug/m3 units
    int8_t   pm4_0_delta_ug;        // 1.0 ug/m3 units (from ug10 / 10)
    int16_t  pm10_delta_ug10;       // 0.1 ug/m3 units
    uint8_t  flags;                 // clamp/overflow indicator bits
};

static constexpr uint8_t DELTA_FLAG_DT_CLAMPED       = 0x01;
static constexpr uint8_t DELTA_FLAG_TEMP_CLAMPED     = 0x02;
static constexpr uint8_t DELTA_FLAG_HUMID_CLAMPED    = 0x04;
static constexpr uint8_t DELTA_FLAG_PM1_CLAMPED      = 0x08;
static constexpr uint8_t DELTA_FLAG_PM2_5_CLAMPED    = 0x10;
static constexpr uint8_t DELTA_FLAG_PM4_CLAMPED      = 0x20;
static constexpr uint8_t DELTA_FLAG_PM10_CLAMPED     = 0x40;

static_assert(sizeof(PktHeader)           ==  5, "PktHeader must be 5 bytes");
static_assert(sizeof(AwakenPayload)       ==  6, "AwakenPayload must be 6 bytes");
static_assert(sizeof(WindowMarkerPayload) == 11, "WindowMarkerPayload must be 11 bytes");
static_assert(sizeof(FullStatePayload)    == 20, "FullStatePayload must be 20 bytes");
static_assert(sizeof(StatusPayload)       == 21, "StatusPayload must be 21 bytes");
static_assert(sizeof(TimeSyncPayload)     ==  8, "TimeSyncPayload must be 8 bytes");
static_assert(sizeof(AckSummaryPayload)   ==  4, "AckSummaryPayload must be 4 bytes");
static_assert(sizeof(DeltaPayload)        == 12, "DeltaPayload must be 12 bytes");
static_assert(sizeof(CmdCalibratePayload) ==  2, "CmdCalibratePayload must be 2 bytes");
static_assert(sizeof(CmdResetPayload)     ==  2, "CmdResetPayload must be 2 bytes");
static_assert(sizeof(CmdSetTxPowerPayload) == 3, "CmdSetTxPowerPayload must be 3 bytes");
static_assert(sizeof(CmdAckPayload)       ==  6, "CmdAckPayload must be 6 bytes");

static constexpr uint8_t kBundleMaxDeltas = 14;

// LoRa payload sizes (no UART framing). Each includes a trailing CRC-8 byte.
static constexpr size_t kAwakenLoRaSize =
    sizeof(PktHeader) + sizeof(AwakenPayload) + 1;                      //  12
// Legacy AWAKEN frame: 4-byte header (no flags byte) + uid_hash-only 4-byte
// payload. Predates both the reset_cause/hang_zone fields and PktHeader::flags,
// so it is decoded against its own header size, not sizeof(PktHeader).
static constexpr size_t kLegacyHeaderSize      = 4;
static constexpr size_t kAwakenPayloadLegacyLen = 4;
static constexpr size_t kAwakenLoRaSizeLegacy =
    kLegacyHeaderSize + kAwakenPayloadLegacyLen + 1;                    //   9
static constexpr size_t kStatusLoRaSize =
    sizeof(PktHeader) + sizeof(StatusPayload) + 1;                      //  27
static constexpr size_t kWindowMarkerLoRaSize =
    sizeof(PktHeader) + sizeof(WindowMarkerPayload) + 1;                //  17
static constexpr size_t kTimeSyncLoRaSize =
    sizeof(PktHeader) + sizeof(TimeSyncPayload) + 1;                    //  14
static constexpr size_t kAckSummaryLoRaSize =
    sizeof(PktHeader) + sizeof(AckSummaryPayload) + 1;                  //  10
static constexpr size_t kCmdCalibrateLoRaSize =
    sizeof(PktHeader) + sizeof(CmdCalibratePayload) + 1;                //   8
static constexpr size_t kCmdResetLoRaSize =
    sizeof(PktHeader) + sizeof(CmdResetPayload) + 1;                    //   8
static constexpr size_t kCmdSetTxPowerLoRaSize =
    sizeof(PktHeader) + sizeof(CmdSetTxPowerPayload) + 1;               //   9
static constexpr size_t kCmdAckLoRaSize =
    sizeof(PktHeader) + sizeof(CmdAckPayload) + 1;                      //  12
static constexpr size_t kFullStateLoRaSize =
    sizeof(PktHeader) + sizeof(FullStatePayload) + 1;                   //  26
static constexpr size_t kMaxBundleLoRaSize =
    sizeof(PktHeader) + sizeof(FullStatePayload) + 1 +
    kBundleMaxDeltas * sizeof(DeltaPayload) + 1;                        // 195

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

// ---------- encode: raw LoRa AWAKEN payload (6 bytes) ----------

inline uint8_t encodeAwakenPayload(
    uint8_t node_id, uint8_t seq,
    const AwakenPayload& awaken,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    if (buf_size < kAwakenLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_AWAKEN;
    hdr.node_id  = node_id;
    hdr.seq      = seq;
    hdr.flags    = flags;
    memcpy(buf, &hdr, sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &awaken, sizeof(AwakenPayload));
    buf[sizeof(PktHeader) + sizeof(AwakenPayload)] =
        crc8(buf, sizeof(PktHeader) + sizeof(AwakenPayload));
    return static_cast<uint8_t>(kAwakenLoRaSize);
}

// ---------- encode: raw LoRa STATUS payload (27 bytes) ----------

inline uint8_t encodeStatusPayload(
    uint8_t node_id, uint8_t seq,
    const StatusPayload& sp,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    if (buf_size < kStatusLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_STATUS;
    hdr.node_id  = node_id;
    hdr.seq      = seq;
    hdr.flags    = flags;
    memcpy(buf,                    &hdr, sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &sp,  sizeof(StatusPayload));
    buf[sizeof(PktHeader) + sizeof(StatusPayload)] = crc8(buf, sizeof(PktHeader) + sizeof(StatusPayload));
    return static_cast<uint8_t>(kStatusLoRaSize);
}

// ---------- encode: raw LoRa WINDOW_BEGIN / WINDOW_END payload (17 bytes) ----------
//
// pktType must be PKT_WINDOW_BEGIN or PKT_WINDOW_END. seq is fixed at 0: these
// frames are outside the ack bitmap and must never advance the telemetry
// sequence (see WindowMarkerPayload). window_id is the sequence that identifies
// them.

inline uint8_t encodeWindowMarkerPayload(
    uint8_t pkt_type, uint8_t node_id,
    const WindowMarkerPayload& marker,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    if (buf_size < kWindowMarkerLoRaSize) return 0;
    if (pkt_type != PKT_WINDOW_BEGIN && pkt_type != PKT_WINDOW_END) return 0;

    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = pkt_type;
    hdr.node_id  = node_id;
    hdr.seq      = 0;
    hdr.flags    = flags;
    memcpy(buf,                    &hdr,    sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &marker, sizeof(WindowMarkerPayload));
    buf[sizeof(PktHeader) + sizeof(WindowMarkerPayload)] =
        crc8(buf, sizeof(PktHeader) + sizeof(WindowMarkerPayload));
    return static_cast<uint8_t>(kWindowMarkerLoRaSize);
}

// ---------- encode: raw LoRa TIME_SYNC payload (12 bytes, base -> nodes broadcast) ----------

inline uint8_t encodeTimeSyncPayload(
    uint8_t node_id, uint8_t seq,
    const TimeSyncPayload& ts,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    if (buf_size < kTimeSyncLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_TIME_SYNC;
    hdr.node_id  = node_id;
    hdr.seq      = seq;
    hdr.flags    = flags;
    memcpy(buf,                    &hdr, sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &ts,  sizeof(TimeSyncPayload));
    buf[sizeof(PktHeader) + sizeof(TimeSyncPayload)] = crc8(buf, sizeof(PktHeader) + sizeof(TimeSyncPayload));
    return static_cast<uint8_t>(kTimeSyncLoRaSize);
}

inline uint8_t encodeTimeSyncPayload(
    uint8_t seq,
    const TimeSyncPayload& ts,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    return encodeTimeSyncPayload(0, seq, ts, buf, buf_size, flags);
}

inline uint8_t encodeAckSummaryPayload(
    uint8_t seq,
    const AckSummaryPayload& as,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    if (buf_size < kAckSummaryLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_ACK_SUMMARY;
    hdr.node_id  = 0;
    hdr.seq      = seq;
    hdr.flags    = flags;
    memcpy(buf,                    &hdr, sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &as, sizeof(AckSummaryPayload));
    buf[sizeof(PktHeader) + sizeof(AckSummaryPayload)] = crc8(buf, sizeof(PktHeader) + sizeof(AckSummaryPayload));
    return static_cast<uint8_t>(kAckSummaryLoRaSize);
}

inline uint8_t encodeCmdCalibratePayload(
    uint8_t seq,
    const CmdCalibratePayload& cmd,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    if (buf_size < kCmdCalibrateLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_CMD_CALIBRATE;
    hdr.node_id  = 0;
    hdr.seq      = seq;
    hdr.flags    = flags;
    memcpy(buf,                    &hdr, sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &cmd, sizeof(CmdCalibratePayload));
    buf[sizeof(PktHeader) + sizeof(CmdCalibratePayload)] =
        crc8(buf, sizeof(PktHeader) + sizeof(CmdCalibratePayload));
    return static_cast<uint8_t>(kCmdCalibrateLoRaSize);
}

inline uint8_t encodeCmdResetPayload(
    uint8_t seq,
    const CmdResetPayload& cmd,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    if (buf_size < kCmdResetLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_CMD_RESET;
    hdr.node_id  = 0;
    hdr.seq      = seq;
    hdr.flags    = flags;
    memcpy(buf,                    &hdr, sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &cmd, sizeof(CmdResetPayload));
    buf[sizeof(PktHeader) + sizeof(CmdResetPayload)] =
        crc8(buf, sizeof(PktHeader) + sizeof(CmdResetPayload));
    return static_cast<uint8_t>(kCmdResetLoRaSize);
}

// node_id lives in the payload, not the header — same convention as
// encodeCmdCalibratePayload/encodeCmdResetPayload above, which also leave
// hdr.node_id at 0 on base-originated commands.
inline uint8_t encodeCmdSetTxPowerPayload(
    uint8_t seq,
    const CmdSetTxPowerPayload& cmd,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    if (buf_size < kCmdSetTxPowerLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_CMD_SET_TX_POWER;
    hdr.node_id  = 0;
    hdr.seq      = seq;
    hdr.flags    = flags;
    memcpy(buf,                    &hdr, sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &cmd, sizeof(CmdSetTxPowerPayload));
    buf[sizeof(PktHeader) + sizeof(CmdSetTxPowerPayload)] =
        crc8(buf, sizeof(PktHeader) + sizeof(CmdSetTxPowerPayload));
    return static_cast<uint8_t>(kCmdSetTxPowerLoRaSize);
}

inline uint8_t encodeCmdAckPayload(
    uint8_t node_id, uint8_t seq,
    const CmdAckPayload& ack,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    if (buf_size < kCmdAckLoRaSize) return 0;
    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_CMD_ACK;
    hdr.node_id  = node_id;
    hdr.seq      = seq;
    hdr.flags    = flags;
    memcpy(buf,                    &hdr, sizeof(PktHeader));
    memcpy(buf + sizeof(PktHeader), &ack, sizeof(CmdAckPayload));
    buf[sizeof(PktHeader) + sizeof(CmdAckPayload)] =
        crc8(buf, sizeof(PktHeader) + sizeof(CmdAckPayload));
    return static_cast<uint8_t>(kCmdAckLoRaSize);
}

// ---------- encode: raw LoRa BUNDLE payload ----------
//
// Output: [PktHeader:4][FullStatePayload:20][n_deltas:1][DeltaPayload×n]
// Returns bytes written, or 0 on failure.

inline uint8_t encodeBundlePayload(
    uint8_t node_id, uint8_t seq,
    const FullStatePayload& ref,
    const DeltaPayload* deltas, uint8_t n_deltas,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    if (n_deltas > kBundleMaxDeltas) return 0;

    const size_t len = sizeof(PktHeader) + sizeof(FullStatePayload)
                     + 1 + static_cast<size_t>(n_deltas) * sizeof(DeltaPayload) + 1; // +1 CRC
    if (buf_size < len) return 0;

    PktHeader hdr;
    hdr.magic    = PKT_MAGIC;
    hdr.pkt_type = PKT_BUNDLE;
    hdr.node_id  = node_id;
    hdr.seq      = seq;
    hdr.flags    = flags;

    size_t off = 0;
    memcpy(buf + off, &hdr, sizeof(PktHeader));        off += sizeof(PktHeader);
    memcpy(buf + off, &ref, sizeof(FullStatePayload)); off += sizeof(FullStatePayload);
    buf[off++] = n_deltas;
    for (uint8_t i = 0; i < n_deltas; ++i) {
        memcpy(buf + off, &deltas[i], sizeof(DeltaPayload));
        off += sizeof(DeltaPayload);
    }
    buf[off] = crc8(buf, off);

    return static_cast<uint8_t>(len);
}

// ---------- encode: TIME_SYNC UART frame (Jetson -> base, 18 bytes) ----------

inline size_t encodeTimeSyncFrame(
    uint8_t node_id, uint8_t seq,
    const TimeSyncPayload& ts,
    uint8_t* buf, size_t buf_size,
    uint8_t flags = 0)
{
    static constexpr size_t kDataLen  = kTimeSyncLoRaSize;
    static constexpr size_t kFrameLen = 2 + 1 + kDataLen + 1;

    if (buf_size < kFrameLen) return 0;

    buf[0] = FRAME_M0;
    buf[1] = FRAME_M1;
    buf[2] = static_cast<uint8_t>(kDataLen);

    const uint8_t encoded = encodeTimeSyncPayload(
        node_id, seq, ts, buf + 3, buf_size - 3, flags);
    if (encoded != kDataLen) return 0;

    buf[3 + kDataLen] = crc8(buf + 2, 1 + kDataLen);
    return kFrameLen;
}

// ---------- encode: base -> Jetson USB-serial frame (variable length) ----------

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
    // Minimum: header + FullState + n_deltas byte + CRC byte (0 deltas)
    const size_t kMin = sizeof(PktHeader) + sizeof(FullStatePayload) + 1 + 1;
    if (len < kMin) return false;

    // Verify CRC — covers all bytes except the trailing CRC byte
    if (crc8(raw, len - 1) != raw[len - 1]) return false;

    size_t off = 0;
    memcpy(&hdr_out, raw + off, sizeof(PktHeader)); off += sizeof(PktHeader);
    if (hdr_out.magic != PKT_MAGIC || hdr_out.pkt_type != PKT_BUNDLE) return false;

    memcpy(&ref_out, raw + off, sizeof(FullStatePayload)); off += sizeof(FullStatePayload);
    delta_count_out = raw[off++];
    if (delta_count_out > kBundleMaxDeltas) return false;
    // Remaining data bytes: delta_count * sizeof(DeltaPayload), then 1 CRC byte
    if (len < off + static_cast<size_t>(delta_count_out) * sizeof(DeltaPayload) + 1) return false;

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
    if (crc8(raw, len - 1) != raw[len - 1]) return false;
    memcpy(&hdr_out, raw,                    sizeof(PktHeader));
    memcpy(&sp_out,  raw + sizeof(PktHeader), sizeof(StatusPayload));
    return hdr_out.magic == PKT_MAGIC && hdr_out.pkt_type == PKT_STATUS;
}

// Length-adaptive: accepts both the current 12-byte frame (5-byte header,
// 6-byte payload with reset_cause/hang_zone) and the legacy 9-byte frame
// (4-byte header without the flags byte, 4-byte uid_hash-only payload). The
// two layouts are told apart by length, and each is CRC-checked over its own
// frame length. Missing fields default to 0 (flags=0 / reset_cause=0 /
// hang_zone=ZONE_UNKNOWN).
//
// Caveat: a node old enough to send the legacy frame also encodes BUNDLE and
// STATUS with the 4-byte header, which this build cannot decode. Accepting its
// AWAKEN only keeps the handshake and node-assignment logs readable — it does
// not make a pre-flags node usable. Flash nodes, base, and the Jetson together.
inline bool decodeAwaken(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, AwakenPayload& awaken_out)
{
    size_t header_len;
    size_t payload_len;
    if (len >= kAwakenLoRaSize) {
        header_len  = sizeof(PktHeader);
        payload_len = sizeof(AwakenPayload);      // 5 + 6 + 1 = 12
    } else if (len >= kAwakenLoRaSizeLegacy) {
        header_len  = kLegacyHeaderSize;
        payload_len = kAwakenPayloadLegacyLen;    // 4 + 4 + 1 =  9
    } else {
        return false;
    }

    const size_t frame_len = header_len + payload_len + 1;
    if (crc8(raw, frame_len - 1) != raw[frame_len - 1]) return false;

    hdr_out    = PktHeader{};     // zero-init so a legacy frame reports flags=0
    awaken_out = AwakenPayload{}; // ... and reset_cause/hang_zone = 0
    memcpy(&hdr_out, raw, header_len);
    memcpy(&awaken_out, raw + header_len, payload_len);
    return hdr_out.magic == PKT_MAGIC && hdr_out.pkt_type == PKT_AWAKEN;
}

// Accepts either marker type; the caller reads hdr_out.pkt_type to tell a window
// open from a window close.
inline bool decodeWindowMarker(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, WindowMarkerPayload& marker_out)
{
    if (len < kWindowMarkerLoRaSize) return false;
    if (crc8(raw, kWindowMarkerLoRaSize - 1) != raw[kWindowMarkerLoRaSize - 1]) return false;
    memcpy(&hdr_out,    raw,                    sizeof(PktHeader));
    memcpy(&marker_out, raw + sizeof(PktHeader), sizeof(WindowMarkerPayload));
    return hdr_out.magic == PKT_MAGIC &&
           (hdr_out.pkt_type == PKT_WINDOW_BEGIN ||
            hdr_out.pkt_type == PKT_WINDOW_END);
}

inline bool decodeTimeSync(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, TimeSyncPayload& ts_out)
{
    if (len < kTimeSyncLoRaSize) return false;
    if (crc8(raw, len - 1) != raw[len - 1]) return false;
    memcpy(&hdr_out, raw,                    sizeof(PktHeader));
    memcpy(&ts_out,  raw + sizeof(PktHeader), sizeof(TimeSyncPayload));
    return hdr_out.magic == PKT_MAGIC && hdr_out.pkt_type == PKT_TIME_SYNC;
}

inline bool decodeAckSummary(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, AckSummaryPayload& as_out)
{
    if (len < kAckSummaryLoRaSize) return false;
    if (crc8(raw, len - 1) != raw[len - 1]) return false;
    memcpy(&hdr_out, raw,                    sizeof(PktHeader));
    memcpy(&as_out,  raw + sizeof(PktHeader), sizeof(AckSummaryPayload));
    return hdr_out.magic == PKT_MAGIC && hdr_out.pkt_type == PKT_ACK_SUMMARY;
}

inline bool decodeCmdCalibrate(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, CmdCalibratePayload& cmd_out)
{
    if (len < kCmdCalibrateLoRaSize) return false;
    if (crc8(raw, len - 1) != raw[len - 1]) return false;
    memcpy(&hdr_out, raw, sizeof(PktHeader));
    memcpy(&cmd_out, raw + sizeof(PktHeader), sizeof(CmdCalibratePayload));
    return hdr_out.magic == PKT_MAGIC && hdr_out.pkt_type == PKT_CMD_CALIBRATE;
}

inline bool decodeCmdReset(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, CmdResetPayload& cmd_out)
{
    if (len < kCmdResetLoRaSize) return false;
    if (crc8(raw, len - 1) != raw[len - 1]) return false;
    memcpy(&hdr_out, raw, sizeof(PktHeader));
    memcpy(&cmd_out, raw + sizeof(PktHeader), sizeof(CmdResetPayload));
    return hdr_out.magic == PKT_MAGIC && hdr_out.pkt_type == PKT_CMD_RESET;
}

inline bool decodeCmdSetTxPower(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, CmdSetTxPowerPayload& cmd_out)
{
    if (len < kCmdSetTxPowerLoRaSize) return false;
    if (crc8(raw, len - 1) != raw[len - 1]) return false;
    memcpy(&hdr_out, raw, sizeof(PktHeader));
    memcpy(&cmd_out, raw + sizeof(PktHeader), sizeof(CmdSetTxPowerPayload));
    return hdr_out.magic == PKT_MAGIC && hdr_out.pkt_type == PKT_CMD_SET_TX_POWER;
}

inline bool decodeCmdAck(
    const uint8_t* raw, size_t len,
    PktHeader& hdr_out, CmdAckPayload& ack_out)
{
    if (len < kCmdAckLoRaSize) return false;
    if (crc8(raw, len - 1) != raw[len - 1]) return false;
    memcpy(&hdr_out, raw, sizeof(PktHeader));
    memcpy(&ack_out, raw + sizeof(PktHeader), sizeof(CmdAckPayload));
    return hdr_out.magic == PKT_MAGIC && hdr_out.pkt_type == PKT_CMD_ACK;
}

} // namespace BinaryPacket
