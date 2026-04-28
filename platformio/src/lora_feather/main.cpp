#include <Arduino.h>
#include <RH_RF95.h>
#include <RHReliableDatagram.h>
#include "shared/BinaryPacket.h"

// LoRa radio pins (Adafruit Feather M0 RFM95)
static constexpr int   RFM95_CS   = 8;
static constexpr int   RFM95_INT  = 3;
static constexpr int   RFM95_RST  = 4;
static constexpr float RFM95_FREQ = 915.0f;

// RHReliableDatagram address scheme:
//   base station = BASE_ADDR (0x01)
//   each node    = its compile-time NODE_ID
static constexpr uint8_t  BASE_ADDR       = 0x01;
static constexpr uint8_t  LORA_RETRIES    = 1;
// 100 ms ACK timeout — ACK typically arrives in ~35 ms (base processes + sends short ACK).
// Bundle TX time ≈ 292 ms for N_δ=7 (177-byte payload). Worst-case 1 retry (2 total attempts):
//   2 × (292 ms TX + 100 ms timeout) = 784 ms < 860 ms active window (900 ms slot - 2×20 ms guard).
static constexpr uint16_t LORA_TIMEOUT_MS = 100;

RH_RF95 rf95(RFM95_CS, RFM95_INT);

void resetRadio() {
    pinMode(RFM95_RST, OUTPUT);
    digitalWrite(RFM95_RST, HIGH); delay(10);
    digitalWrite(RFM95_RST, LOW);  delay(10);
    digitalWrite(RFM95_RST, HIGH); delay(10);
}

// ---------------------------------------------------------------------------
// Shared UART frame receive state machine — used by both NODE and BASE sections.
// ---------------------------------------------------------------------------

enum FrameState : uint8_t {
    FS_WAIT_M0 = 0,
    FS_WAIT_M1,
    FS_WAIT_LEN,
    FS_READ_DATA,
    FS_CHECK_CRC,
};

// ---------------------------------------------------------------------------
#if defined(LORA_NODE)
// ---------------------------------------------------------------------------
// NODE MODE — TDMA
//
//   TDMA frame structure (kNumSlots slots, each kSlotWidthMs wide):
//
//     |--guard--|---TX window---|--guard--|  ← one slot
//     |  node 1  |  node 2  |  ...  |  node 1  |...
//     0                              kFramePeriodMs
//
//   session_time is derived from the last TIME_SYNC broadcast received from
//   the base station (driven by the Jetson). Drift between syncs (30 s interval)
//   is ±1.5 ms at 50 ppm — well within the 20 ms guard bands.
//
//   Stale-sync fallback: if no TIME_SYNC arrives for kSyncStaleMs (5 min),
//   TDMA is suspended and the node reverts to immediate TX to ensure data flows.
//
//   Queue is drop-oldest when full: always holds the most recent sensor data.
//
// Build flags required: -D LORA_NODE=1  -D NODE_ID=<n>  -D NUM_SLOTS=<m>
// ---------------------------------------------------------------------------

#ifndef NODE_ID
  #error "NODE_ID must be defined in platformio.ini when using LORA_NODE"
#endif

#ifndef NUM_SLOTS
  #define NUM_SLOTS 2
#endif

// TDMA parameters — identical across all nodes in the network.
// Slot width sized for N_δ=7 bundles (177-byte LoRa payload, T_tx≈292 ms) with 1 retry:
//   W_min = 2 × (292 + 100) + 40 = 824 ms → rounded up to 900 ms for margin.
static constexpr uint32_t kSlotWidthMs   = 900;
static constexpr uint32_t kNumSlots      = static_cast<uint32_t>(NUM_SLOTS);
static constexpr uint32_t kFramePeriodMs = kSlotWidthMs * kNumSlots;
static constexpr uint32_t kGuardMs       = 20;    // silence at each slot boundary
static constexpr uint32_t kSyncStaleMs   = 300000; // 5 min without sync → fallback mode

// This node's slot index (0-based). NODE_ID=1 → slot 0, NODE_ID=2 → slot 1, etc.
static constexpr uint8_t kMySlot = static_cast<uint8_t>((NODE_ID - 1) % NUM_SLOTS);

RHReliableDatagram manager(rf95, static_cast<uint8_t>(NODE_ID));

// ---- TX ring buffer (drop-oldest when full) ----

static constexpr uint8_t kQueueDepth = 4;

struct TxEntry {
    uint8_t payload[BinaryPacket::kMaxLoRaPayloadSize];  // up to 177 bytes for N_δ=7 bundle
    uint8_t len;
};

static TxEntry txQueue[kQueueDepth];
static uint8_t qHead  = 0;
static uint8_t qTail  = 0;
static uint8_t qCount = 0;

// Always succeeds. Drops the oldest entry when the queue is full so that
// the most recent sensor data is always preserved.
static void enqueue(const uint8_t* payload, uint8_t len) {
    if (qCount >= kQueueDepth) {
        // Evict oldest
        qHead = (qHead + 1) % kQueueDepth;
        qCount--;
    }
    memcpy(txQueue[qTail].payload, payload, len);
    txQueue[qTail].len = len;
    qTail = (qTail + 1) % kQueueDepth;
    qCount++;
}

static bool dequeue(uint8_t* payload, uint8_t& len_out) {
    if (qCount == 0) return false;
    len_out = txQueue[qHead].len;
    memcpy(payload, txQueue[qHead].payload, len_out);
    qHead = (qHead + 1) % kQueueDepth;
    qCount--;
    return true;
}

// ---- TDMA clock (Feather-local session time) ----

static uint32_t tdmaSyncSessionMs = 0;    // session_time_ms from last TIME_SYNC
static uint32_t tdmaSyncLocalMs   = 0;    // millis() when that sync was received
static bool     tdmaHasSynced     = false;
static uint32_t tdmaLastSlotTx    = 0xFFFFFFFFu; // slot index of last TX attempt

// Returns the estimated current session_time_ms.
static uint32_t tdmaSessionNow() {
    return tdmaSyncSessionMs + (millis() - tdmaSyncLocalMs);
}

// Returns true if the node should transmit right now.
// Falls back to immediate TX if no valid sync exists or sync has gone stale.
static bool tdmaMyTurn(uint32_t& slotIndexOut) {
    const uint32_t localMs = millis();
    const bool stale = tdmaHasSynced && ((localMs - tdmaSyncLocalMs) > kSyncStaleMs);

    if (!tdmaHasSynced || stale) {
        // No valid sync — transmit without slot restriction.
        // Use a pseudo slot index that always looks "new" so 1-per-call limit still works.
        slotIndexOut = localMs / kSlotWidthMs;
        return true;
    }

    const uint32_t sessionMs = tdmaSyncSessionMs + (localMs - tdmaSyncLocalMs);
    const uint32_t slotIndex = sessionMs / kSlotWidthMs;
    const uint32_t posInSlot = sessionMs % kSlotWidthMs;
    const uint32_t whichSlot = slotIndex % kNumSlots;

    slotIndexOut = slotIndex;

    if (whichSlot != kMySlot)                                    return false; // not our slot
    if (posInSlot < kGuardMs)                                    return false; // leading guard
    if (posInSlot >= kSlotWidthMs - kGuardMs)                    return false; // trailing guard
    return true;
}

// ---- UART receive state machine ----
// frameBuf holds [len_byte][LoRa payload bytes].
// Max bundle UART frame data: PktHeader(4)+FullStatePayload(32)+n_deltas(1)+7×DeltaPayload(140) = 177.
// frameBuf needs 1 (len byte) + 177 = 178 bytes minimum; use 200 for headroom.

static FrameState frameState      = FS_WAIT_M0;
static uint8_t    frameBuf[200];
static uint8_t    frameExpectedLen = 0;
static uint8_t    framePos         = 0;

// Forward declarations
void processFrame(const uint8_t* data, uint8_t len);
void sendAck(uint8_t seq);
void drainTxQueue();
void sendPayload(const uint8_t* payload, uint8_t len);
void checkIncomingLora();

void setup() {
    Serial.begin(115200);
    Serial1.begin(115200);
    delay(1000);

    resetRadio();
    if (!manager.init()) {
        Serial.println("[LORA] init failed");
        while (1) { delay(100); }
    }
    if (!rf95.setFrequency(RFM95_FREQ)) {
        Serial.println("[LORA] setFrequency failed");
        while (1) { delay(100); }
    }
    rf95.setTxPower(13, false);
    manager.setRetries(LORA_RETRIES);
    manager.setTimeout(LORA_TIMEOUT_MS);
    rf95.setCADTimeout(10);  // sense channel for up to 10 ms before each TX

    Serial.print("[TDMA] Node id=");
    Serial.print(NODE_ID);
    Serial.print(" slot=");
    Serial.print(kMySlot);
    Serial.print("/");
    Serial.print(kNumSlots);
    Serial.print(" slotWidth=");
    Serial.print(kSlotWidthMs);
    Serial.print("ms frame=");
    Serial.print(kFramePeriodMs);
    Serial.println("ms");
    Serial.println("[TDMA] Waiting for TIME_SYNC before first TX...");
}

void loop() {
    // Phase 1: Read bytes from ESP32 UART.
    // processFrame() enqueues + ACKs immediately. No blocking TX here.
    while (Serial1.available() > 0) {
        const uint8_t b = static_cast<uint8_t>(Serial1.read());

        switch (frameState) {

            case FS_WAIT_M0:
                if (b == BinaryPacket::FRAME_M0) frameState = FS_WAIT_M1;
                break;

            case FS_WAIT_M1:
                frameState = (b == BinaryPacket::FRAME_M1) ? FS_WAIT_LEN : FS_WAIT_M0;
                break;

            case FS_WAIT_LEN:
                frameExpectedLen = b;
                framePos = 0;
                frameBuf[framePos++] = b;
                if (frameExpectedLen == 0 ||
                    static_cast<size_t>(1 + frameExpectedLen) > sizeof(frameBuf)) {
                    Serial.println("[UART] bad length, resync");
                    frameState = FS_WAIT_M0;
                } else {
                    frameState = FS_READ_DATA;
                }
                break;

            case FS_READ_DATA:
                frameBuf[framePos++] = b;
                if (framePos == static_cast<uint8_t>(1 + frameExpectedLen)) {
                    frameState = FS_CHECK_CRC;
                }
                break;

            case FS_CHECK_CRC: {
                const uint8_t computed = BinaryPacket::crc8(frameBuf, 1 + frameExpectedLen);
                if (b == computed) {
                    processFrame(frameBuf + 1, frameExpectedLen);
                } else {
                    Serial.print("[UART] CRC mismatch: got ");
                    Serial.print(b, HEX);
                    Serial.print(" expected ");
                    Serial.println(computed, HEX);
                }
                frameState = FS_WAIT_M0;
                break;
            }
        }
    }

    // Phase 2: TDMA-gated TX.
    drainTxQueue();

    // Phase 3: Non-blocking check for TIME_SYNC broadcast.
    checkIncomingLora();
}

void processFrame(const uint8_t* data, uint8_t len) {
    if (len < sizeof(BinaryPacket::PktHeader)) {
        Serial.println("[UART] frame too short");
        Serial1.println("ERR,too_short");
        return;
    }

    BinaryPacket::PktHeader hdr{};
    memcpy(&hdr, data, sizeof(hdr));

    if (hdr.magic != BinaryPacket::PKT_MAGIC ||
        (hdr.pkt_type != BinaryPacket::PKT_FULL_STATE &&
         hdr.pkt_type != BinaryPacket::PKT_BUNDLE)) {
        Serial.println("[UART] bad magic or type");
        Serial1.println("ERR,bad_pkt");
        return;
    }

    sendAck(hdr.seq);  // ACK to ESP32 immediately; LoRa outcome is irrelevant to it

    enqueue(data, len);  // always succeeds; evicts oldest if full
    Serial.print("[TX] enqueued type=");
    Serial.print(hdr.pkt_type == BinaryPacket::PKT_BUNDLE ? "BUNDLE" : "FULL");
    Serial.print(" seq=");
    Serial.print(hdr.seq);
    Serial.print(" len=");
    Serial.print(len);
    Serial.print(" q=");
    Serial.println(qCount);
}

void sendAck(uint8_t seq) {
    char line[16];
    snprintf(line, sizeof(line), "ACK,%u", static_cast<unsigned>(seq));
    Serial1.println(line);
}

void drainTxQueue() {
    if (qCount == 0) return;

    uint32_t slotIndex = 0;
    if (!tdmaMyTurn(slotIndex)) return;     // not our slot or guard period
    if (slotIndex == tdmaLastSlotTx) return; // already attempted TX in this slot

    // Claim the slot before dequeue so we don't retry even if dequeue is empty.
    tdmaLastSlotTx = slotIndex;

    uint8_t payload[BinaryPacket::kMaxLoRaPayloadSize];
    uint8_t payloadLen = 0;
    if (!dequeue(payload, payloadLen)) return;

    sendPayload(payload, payloadLen);
}

void sendPayload(const uint8_t* payload, uint8_t len) {
    BinaryPacket::PktHeader hdr;
    memcpy(&hdr, payload, sizeof(hdr));

    const uint32_t sessionNow = tdmaHasSynced ? tdmaSessionNow() : millis();
    Serial.print("[LORA TX] type=");
    Serial.print(hdr.pkt_type == BinaryPacket::PKT_BUNDLE ? "BUNDLE" : "FULL");
    Serial.print(" seq=");
    Serial.print(hdr.seq);
    Serial.print(" slot=");
    Serial.print(kMySlot);
    Serial.print(" session_ms=");
    Serial.print(sessionNow);
    Serial.print(" len=");
    Serial.print(len);
    Serial.print(" q_remaining=");
    Serial.print(qCount);
    Serial.println(" sending...");

    const uint32_t retxBefore = manager.retransmissions();
    const bool ok = manager.sendtoWait(
        const_cast<uint8_t*>(payload),
        len,
        BASE_ADDR);
    const uint32_t retries = manager.retransmissions() - retxBefore;

    if (ok) {
        Serial.print("[LORA TX] seq=");
        Serial.print(hdr.seq);
        Serial.print(" ACKed");
        if (retries > 0) {
            Serial.print(" after ");
            Serial.print(retries);
            Serial.print(retries == 1 ? " retry" : " retries");
        }
        Serial.println();
    } else {
        Serial.print("[LORA TX] seq=");
        Serial.print(hdr.seq);
        Serial.print(" FAILED after ");
        Serial.print(LORA_RETRIES);
        Serial.println(" retries — dropped");
    }
}

void checkIncomingLora() {
    if (!manager.available()) return;

    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len  = sizeof(buf);
    uint8_t from = 0;

    if (!manager.recvfromAck(buf, &len, &from)) return;

    // Only act on TIME_SYNC broadcasts; ignore everything else.
    BinaryPacket::PktHeader       hdr{};
    BinaryPacket::TimeSyncPayload ts{};
    if (!BinaryPacket::decodeTimeSync(buf, len, hdr, ts)) return;

    // Update the local TDMA clock.
    tdmaSyncSessionMs = ts.session_time_ms;
    tdmaSyncLocalMs   = millis();
    tdmaHasSynced     = true;

    Serial.print("[TDMA] sync: session_id=");
    Serial.print(ts.session_id);
    Serial.print(" session_ms=");
    Serial.print(ts.session_time_ms);
    Serial.print(" slot=");
    Serial.print(ts.session_time_ms % kFramePeriodMs / kSlotWidthMs);
    Serial.println();

    // Forward to ESP32 so it can update its session_time offset for packet timestamps.
    uint8_t frame[20];
    const size_t frame_len = BinaryPacket::encodeTimeSyncFrame(0, hdr.seq, ts, frame, sizeof(frame));
    if (frame_len > 0) Serial1.write(frame, frame_len);
}

// ---------------------------------------------------------------------------
#elif defined(LORA_BASE_STATION)
// ---------------------------------------------------------------------------
// BASE STATION MODE
//   - Listens for LoRa packets from any node via RHReliableDatagram
//   - recvfromAck() auto-ACKs each received telemetry packet back to the sender
//   - Validates packet magic / type before forwarding
//   - Forwards to Jetson over UART (Serial1) with RSSI prepended (41-byte frame)
//
//   - ALSO reads TIME_SYNC commands from Jetson over UART (Serial1, bidirectional)
//   - When a valid TIME_SYNC frame arrives from Jetson, broadcasts it over LoRa
//     to all nodes using RH_BROADCAST_ADDRESS (fire-and-forget, no ACK expected)
//
// The base station has no TDMA logic — it receives whenever and auto-ACKs.
//
// Build flags required: -D LORA_BASE_STATION=1
// ---------------------------------------------------------------------------

RHReliableDatagram manager(rf95, BASE_ADDR);

// ---- Jetson → base UART frame state machine (TIME_SYNC commands) ----

static FrameState jetsonState   = FS_WAIT_M0;
static uint8_t   jetsonBuf[20];
static uint8_t   jetsonExpected = 0;
static uint8_t   jetsonPos      = 0;

static bool    pendingTimeSync = false;
static uint8_t timeSyncBuf[BinaryPacket::kTimeSyncLoRaSize];

void handleJetsonByte(uint8_t b);
void broadcastTimeSync();

void setup() {
    Serial.begin(115200);
    Serial1.begin(115200);
    delay(1000);

    resetRadio();
    if (!manager.init()) {
        Serial.println("[LORA] init failed");
        while (1) { delay(100); }
    }
    if (!rf95.setFrequency(RFM95_FREQ)) {
        Serial.println("[LORA] setFrequency failed");
        while (1) { delay(100); }
    }
    rf95.setTxPower(13, false);
    manager.setRetries(LORA_RETRIES);
    manager.setTimeout(LORA_TIMEOUT_MS);
    rf95.setCADTimeout(10);  // sense channel for up to 10 ms before each TX

    Serial.println("[LORA] Base station ready (auto-ACK, TIME_SYNC relay enabled)");
}

void loop() {
    // Read TIME_SYNC commands from Jetson.
    while (Serial1.available() > 0) {
        handleJetsonByte(static_cast<uint8_t>(Serial1.read()));
    }

    // Broadcast pending TIME_SYNC when the radio is not mid-receive.
    if (pendingTimeSync && !manager.available()) {
        broadcastTimeSync();
        pendingTimeSync = false;
    }

    // Receive telemetry from nodes.
    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len  = sizeof(buf);
    uint8_t from = 0;

    if (!manager.recvfromAck(buf, &len, &from)) return;

    const int8_t rssi = static_cast<int8_t>(rf95.lastRssi());

    // Validate: accept FULL_STATE and BUNDLE packets; reject anything else.
    if (len < sizeof(BinaryPacket::PktHeader)) {
        Serial.println("[LORA] packet too short");
        return;
    }
    BinaryPacket::PktHeader hdr{};
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != BinaryPacket::PKT_MAGIC ||
        (hdr.pkt_type != BinaryPacket::PKT_FULL_STATE &&
         hdr.pkt_type != BinaryPacket::PKT_BUNDLE)) {
        Serial.print("[LORA] invalid packet from=0x");
        Serial.print(from, HEX);
        Serial.print(" type=0x");
        Serial.print(hdr.pkt_type, HEX);
        Serial.print(" len=");
        Serial.println(len);
        return;
    }

    Serial.print("[LORA RX] type=");
    Serial.print(hdr.pkt_type == BinaryPacket::PKT_BUNDLE ? "BUNDLE" : "FULL");
    Serial.print(" node=");
    Serial.print(hdr.node_id);
    Serial.print(" seq=");
    Serial.print(hdr.seq);
    Serial.print(" len=");
    Serial.print(len);
    Serial.print(" rssi=");
    Serial.println(rssi);

    // Frame buffer: 2 preamble + 1 len + 1 rssi + up to 177 LoRa bytes + 1 CRC = 182 bytes max.
    uint8_t frame[200];
    const size_t frame_len =
        BinaryPacket::encodeBaseFrame(rssi, buf, len, frame, sizeof(frame));

    if (frame_len > 0) {
        Serial1.write(frame, frame_len);
    } else {
        Serial.println("[UART] encodeBaseFrame failed");
    }
}

void handleJetsonByte(uint8_t b) {
    switch (jetsonState) {

        case FS_WAIT_M0:
            if (b == BinaryPacket::FRAME_M0) jetsonState = FS_WAIT_M1;
            break;

        case FS_WAIT_M1:
            jetsonState = (b == BinaryPacket::FRAME_M1) ? FS_WAIT_LEN : FS_WAIT_M0;
            break;

        case FS_WAIT_LEN:
            jetsonExpected = b;
            jetsonPos = 0;
            if (jetsonExpected == 0 ||
                static_cast<size_t>(1 + jetsonExpected) > sizeof(jetsonBuf)) {
                jetsonState = FS_WAIT_M0;
            } else {
                jetsonBuf[jetsonPos++] = b;
                jetsonState = FS_READ_DATA;
            }
            break;

        case FS_READ_DATA:
            if (jetsonPos < sizeof(jetsonBuf)) jetsonBuf[jetsonPos++] = b;
            if (jetsonPos == static_cast<uint8_t>(1 + jetsonExpected)) {
                jetsonState = FS_CHECK_CRC;
            }
            break;

        case FS_CHECK_CRC: {
            const uint8_t computed = BinaryPacket::crc8(jetsonBuf, 1 + jetsonExpected);
            if (b == computed) {
                BinaryPacket::PktHeader       hdr{};
                BinaryPacket::TimeSyncPayload ts{};
                if (BinaryPacket::decodeTimeSync(jetsonBuf + 1, jetsonExpected, hdr, ts)) {
                    memcpy(timeSyncBuf, jetsonBuf + 1, BinaryPacket::kTimeSyncLoRaSize);
                    pendingTimeSync = true;
                    Serial.print("[TIME_SYNC] from Jetson: session_id=");
                    Serial.print(ts.session_id);
                    Serial.print(" session_ms=");
                    Serial.println(ts.session_time_ms);
                }
            } else {
                Serial.println("[UART] Jetson CRC mismatch");
            }
            jetsonState = FS_WAIT_M0;
            break;
        }
    }
}

void broadcastTimeSync() {
    manager.sendto(
        timeSyncBuf,
        static_cast<uint8_t>(BinaryPacket::kTimeSyncLoRaSize),
        RH_BROADCAST_ADDRESS);
    Serial.println("[TIME_SYNC] broadcast sent over LoRa");
}

// ---------------------------------------------------------------------------
#else
  #error "Define either LORA_NODE=1 or LORA_BASE_STATION=1 in platformio.ini build_flags"
#endif
