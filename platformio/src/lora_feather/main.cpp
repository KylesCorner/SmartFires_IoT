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
static constexpr uint8_t  LORA_RETRIES    = 5;
static constexpr uint16_t LORA_TIMEOUT_MS = 300;

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
// NODE MODE
//   - Receives binary telemetry frames from ESP32 over UART (Serial1)
//   - Validates CRC, enqueues payload, sends text ACK back to ESP32 immediately
//   - TX queue (kQueueDepth slots) decouples UART receive from LoRa TX
//   - drainTxQueue() sends one slot per loop() call via sendtoWait()
//   - After each TX, checkIncomingLora() checks for TIME_SYNC broadcasts from base
//   - TIME_SYNC received over LoRa is forwarded to ESP32 as a UART binary frame
//
// Build flags required: -D LORA_NODE=1  -D NODE_ID=<n>
// ---------------------------------------------------------------------------

#ifndef NODE_ID
  #error "NODE_ID must be defined in platformio.ini when using LORA_NODE"
#endif

RHReliableDatagram manager(rf95, static_cast<uint8_t>(NODE_ID));

// ---- TX ring buffer ----

static constexpr uint8_t kQueueDepth = 4;

struct TxEntry {
    uint8_t payload[BinaryPacket::kLoRaPayloadSize];
};

static TxEntry txQueue[kQueueDepth];
static uint8_t qHead  = 0;
static uint8_t qTail  = 0;
static uint8_t qCount = 0;

static bool enqueue(const uint8_t* payload) {
    if (qCount >= kQueueDepth) return false;
    memcpy(txQueue[qTail].payload, payload, BinaryPacket::kLoRaPayloadSize);
    qTail = (qTail + 1) % kQueueDepth;
    qCount++;
    return true;
}

static bool dequeue(uint8_t* payload) {
    if (qCount == 0) return false;
    memcpy(payload, txQueue[qHead].payload, BinaryPacket::kLoRaPayloadSize);
    qHead = (qHead + 1) % kQueueDepth;
    qCount--;
    return true;
}

// ---- UART receive state machine ----

static FrameState frameState      = FS_WAIT_M0;
static uint8_t    frameBuf[64];
static uint8_t    frameExpectedLen = 0;
static uint8_t    framePos         = 0;

// Forward declarations
void processFrame(const uint8_t* data, uint8_t len);
void sendAck(uint8_t seq);
void drainTxQueue();
void checkIncomingLora();

void setup() {
    Serial.begin(115200);   // USB — debug output
    Serial1.begin(115200);  // UART — ESP32 link
    delay(1000);

    resetRadio();
    if (!manager.init()) {
        Serial.println("[LORA] RHReliableDatagram init failed");
        while (1) { delay(100); }
    }
    if (!rf95.setFrequency(RFM95_FREQ)) {
        Serial.println("[LORA] setFrequency failed");
        while (1) { delay(100); }
    }
    rf95.setTxPower(13, false);
    manager.setRetries(LORA_RETRIES);
    manager.setTimeout(LORA_TIMEOUT_MS);

    Serial.print("[LORA] Node ready, id=");
    Serial.print(NODE_ID);
    Serial.print(", retries=");
    Serial.print(LORA_RETRIES);
    Serial.print(", timeout=");
    Serial.print(LORA_TIMEOUT_MS);
    Serial.println("ms, queue depth=");
    Serial.println(kQueueDepth);
}

void loop() {
    // Phase 1: Read bytes from ESP32 UART into frame state machine.
    // processFrame() is called when a valid frame arrives; it enqueues + ACKs immediately.
    // sendtoWait() is NOT called here, so this loop is always fast.
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

    // Phase 2: Send one packet from the TX queue over LoRa.
    // sendtoWait() blocks here; UART bytes accumulate in the hardware RX buffer during that time.
    drainTxQueue();

    // Phase 3: Non-blocking check for TIME_SYNC broadcast from the base station.
    checkIncomingLora();
}

void processFrame(const uint8_t* data, uint8_t len) {
    BinaryPacket::PktHeader       hdr{};
    BinaryPacket::FullStatePayload payload{};

    if (!BinaryPacket::decodeFullState(data, len, hdr, payload)) {
        Serial.println("[UART] decode failed");
        Serial1.println("ERR,decode_failed");
        return;
    }

    // ACK immediately — ESP32 does not wait for LoRa outcome
    sendAck(hdr.seq);

    if (!enqueue(data)) {
        Serial.print("[TX] queue full, dropped seq=");
        Serial.println(hdr.seq);
    } else {
        Serial.print("[TX] enqueued seq=");
        Serial.print(hdr.seq);
        Serial.print(" q=");
        Serial.println(qCount);
    }
}

void sendAck(uint8_t seq) {
    char line[16];
    snprintf(line, sizeof(line), "ACK,%u", static_cast<unsigned>(seq));
    Serial1.println(line);
}

void drainTxQueue() {
    if (qCount == 0) return;

    uint8_t payload[BinaryPacket::kLoRaPayloadSize];
    if (!dequeue(payload)) return;

    BinaryPacket::PktHeader hdr;
    memcpy(&hdr, payload, sizeof(hdr));

    Serial.print("[LORA TX] seq=");
    Serial.print(hdr.seq);
    Serial.print(" node=");
    Serial.print(hdr.node_id);
    Serial.print(" q_remaining=");
    Serial.print(qCount);
    Serial.println(" sending...");

    const uint32_t retxBefore = manager.retransmissions();
    const bool ok = manager.sendtoWait(
        payload,
        static_cast<uint8_t>(BinaryPacket::kLoRaPayloadSize),
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
    // Non-blocking: return immediately if no packet is waiting.
    if (!manager.available()) return;

    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len  = sizeof(buf);
    uint8_t from = 0;

    if (!manager.recvfromAck(buf, &len, &from)) return;

    // Only handle TIME_SYNC; ignore everything else (e.g. stray packets from other nodes).
    BinaryPacket::PktHeader      hdr{};
    BinaryPacket::TimeSyncPayload ts{};
    if (!BinaryPacket::decodeTimeSync(buf, len, hdr, ts)) return;

    // Forward to ESP32 as a UART binary frame so it can update its session_time offset.
    uint8_t frame[20];
    const size_t frame_len = BinaryPacket::encodeTimeSyncFrame(0, hdr.seq, ts, frame, sizeof(frame));
    if (frame_len > 0) {
        Serial1.write(frame, frame_len);
        Serial.print("[TIME_SYNC] forwarded to ESP32: session_id=");
        Serial.print(ts.session_id);
        Serial.print(" session_ms=");
        Serial.println(ts.session_time_ms);
    }
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
// Build flags required: -D LORA_BASE_STATION=1
// ---------------------------------------------------------------------------

RHReliableDatagram manager(rf95, BASE_ADDR);

// ---- Jetson → base UART frame state machine (TIME_SYNC commands) ----

static FrameState jetsonState    = FS_WAIT_M0;
static uint8_t   jetsonBuf[20];
static uint8_t   jetsonExpected  = 0;
static uint8_t   jetsonPos       = 0;

static bool    pendingTimeSync = false;
static uint8_t timeSyncBuf[BinaryPacket::kTimeSyncLoRaSize];  // raw LoRa payload to broadcast

void handleJetsonByte(uint8_t b);
void broadcastTimeSync();

void setup() {
    Serial.begin(115200);   // USB — debug output
    Serial1.begin(115200);  // UART — Jetson link (bidirectional)
    delay(1000);

    resetRadio();
    if (!manager.init()) {
        Serial.println("[LORA] RHReliableDatagram init failed");
        while (1) { delay(100); }
    }
    if (!rf95.setFrequency(RFM95_FREQ)) {
        Serial.println("[LORA] setFrequency failed");
        while (1) { delay(100); }
    }
    rf95.setTxPower(13, false);
    manager.setRetries(LORA_RETRIES);
    manager.setTimeout(LORA_TIMEOUT_MS);

    Serial.println("[LORA] Base station ready (RHReliableDatagram, auto-ACK, TIME_SYNC enabled)");
}

void loop() {
    // Read TIME_SYNC commands arriving from the Jetson on Serial1.
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

    BinaryPacket::PktHeader        hdr{};
    BinaryPacket::FullStatePayload payload{};
    if (!BinaryPacket::decodeFullState(buf, len, hdr, payload)) {
        Serial.print("[LORA] invalid packet from=0x");
        Serial.print(from, HEX);
        Serial.print(" len=");
        Serial.println(len);
        return;
    }

    Serial.print("[LORA RX] node=");
    Serial.print(hdr.node_id);
    Serial.print(" from=0x");
    Serial.print(from, HEX);
    Serial.print(" seq=");
    Serial.print(hdr.seq);
    Serial.print(" rssi=");
    Serial.println(rssi);

    // Build UART frame for Jetson (41 bytes: header + rssi + lora payload + crc)
    uint8_t frame[41];
    const size_t frame_len =
        BinaryPacket::encodeBaseFrame(rssi, buf, frame, sizeof(frame));

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
                    // Store the raw 12-byte LoRa payload (PktHeader + TimeSyncPayload)
                    memcpy(timeSyncBuf, jetsonBuf + 1, BinaryPacket::kTimeSyncLoRaSize);
                    pendingTimeSync = true;
                    Serial.print("[TIME_SYNC] from Jetson: session_id=");
                    Serial.print(ts.session_id);
                    Serial.print(" session_ms=");
                    Serial.println(ts.session_time_ms);
                }
            } else {
                Serial.println("[UART] Jetson frame CRC mismatch");
            }
            jetsonState = FS_WAIT_M0;
            break;
        }
    }
}

void broadcastTimeSync() {
    // Fire-and-forget broadcast — nodes receive and handle; no ACK expected.
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
