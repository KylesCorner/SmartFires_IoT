#include <Arduino.h>
#include <RH_RF95.h>
#include "shared/BinaryPacket.h"

// LoRa radio pins (Adafruit Feather M0 RFM95)
static constexpr int   RFM95_CS   = 8;
static constexpr int   RFM95_INT  = 3;
static constexpr int   RFM95_RST  = 4;
static constexpr float RFM95_FREQ = 915.0f;

RH_RF95 rf95(RFM95_CS, RFM95_INT);

void resetRadio() {
    pinMode(RFM95_RST, OUTPUT);
    digitalWrite(RFM95_RST, HIGH); delay(10);
    digitalWrite(RFM95_RST, LOW);  delay(10);
    digitalWrite(RFM95_RST, HIGH); delay(10);
}

// ---------------------------------------------------------------------------
#if defined(LORA_NODE)
// ---------------------------------------------------------------------------
// NODE MODE
//   - Receives binary telemetry frames from ESP32 over UART (Serial1)
//   - Validates CRC, sends text ACK back to ESP32
//   - Forwards raw LoRa payload (PktHeader + FullStatePayload) over radio
//
// Build flags required: -D LORA_NODE=1  -D NODE_ID=<n>
// ---------------------------------------------------------------------------

#ifndef NODE_ID
  #error "NODE_ID must be defined in platformio.ini when using LORA_NODE"
#endif

// Binary frame receiver state machine
enum FrameState : uint8_t {
    FS_WAIT_M0 = 0,
    FS_WAIT_M1,
    FS_WAIT_LEN,
    FS_READ_DATA,
    FS_CHECK_CRC,
};

static FrameState frameState = FS_WAIT_M0;

// buf layout: [len_byte, data[0..len-1]]
// CRC is computed over this entire buffer (len byte + data).
// Sized for the largest expected UART frame (drone sends 35 bytes; data = 31).
static uint8_t  frameBuf[64];
static uint8_t  frameExpectedLen = 0;
static uint8_t  framePos         = 0;  // position in frameBuf (0 = len byte)

void processFrame(const uint8_t* data, uint8_t len);
void sendAck(uint8_t seq);

void setup() {
    Serial.begin(115200);   // USB — debug output
    Serial1.begin(115200);  // UART — ESP32 link
    delay(1000);

    resetRadio();
    if (!rf95.init()) {
        Serial.println("[LORA] RH_RF95 init failed");
        while (1) { delay(100); }
    }
    if (!rf95.setFrequency(RFM95_FREQ)) {
        Serial.println("[LORA] setFrequency failed");
        while (1) { delay(100); }
    }
    rf95.setTxPower(13, false);

    Serial.print("[LORA] Node ready, id=");
    Serial.println(NODE_ID);
}

void loop() {
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
                frameBuf[framePos++] = b;  // frameBuf[0] = len byte (included in CRC)
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
                // frameBuf holds: [len, data[0..frameExpectedLen-1]]
                // done when we have the len byte + all data bytes
                if (framePos == static_cast<uint8_t>(1 + frameExpectedLen)) {
                    frameState = FS_CHECK_CRC;
                }
                break;

            case FS_CHECK_CRC: {
                const uint8_t computed = BinaryPacket::crc8(frameBuf, 1 + frameExpectedLen);
                if (b == computed) {
                    // data starts at frameBuf[1], length = frameExpectedLen
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
}

void processFrame(const uint8_t* data, uint8_t len) {
    BinaryPacket::PktHeader      hdr{};
    BinaryPacket::FullStatePayload payload{};

    if (!BinaryPacket::decodeFullState(data, len, hdr, payload)) {
        Serial.println("[UART] decode failed");
        Serial1.println("ERR,decode_failed");
        return;
    }

    // ACK immediately so ESP32 can send the next packet
    sendAck(hdr.seq);

    // Dump raw bytes about to be sent
    Serial.print("[LORA TX] raw(");
    Serial.print(BinaryPacket::kLoRaPayloadSize);
    Serial.print("):");
    for (size_t i = 0; i < BinaryPacket::kLoRaPayloadSize; i++) {
        Serial.print(' ');
        if (data[i] < 0x10) Serial.print('0');
        Serial.print(data[i], HEX);
    }
    Serial.println();

    // Dump decoded fields
    Serial.print("[LORA TX] hdr magic=0x"); Serial.print(hdr.magic, HEX);
    Serial.print(" type=0x");  Serial.print(hdr.pkt_type, HEX);
    Serial.print(" node=");    Serial.print(hdr.node_id);
    Serial.print(" seq=");     Serial.println(hdr.seq);
    Serial.print("[LORA TX] payload session_ms="); Serial.print(payload.session_time);
    Serial.print(" uptime_ms="); Serial.print(payload.uptime_ms);
    Serial.print(" flags=0x");   Serial.print(payload.sensor_flags, HEX);
    Serial.print(" flame=");     Serial.println(payload.flame);
    Serial.print("[LORA TX]   wind_cms="); Serial.print(payload.wind_cms);
    Serial.print(" temp_cdegc="); Serial.print(payload.temp_cdegc);
    Serial.print(" hum_cpct=");   Serial.print(payload.humidity_cpct);
    Serial.print(" lidar_cm=");   Serial.println(payload.lidar_cm);
    Serial.print("[LORA TX]   lat_e7="); Serial.print(payload.lat_e7);
    Serial.print(" lon_e7=");     Serial.println(payload.lon_e7);

    // Forward raw LoRa payload (header + sensor data, no UART framing overhead)
    if (!rf95.send(data, static_cast<uint8_t>(BinaryPacket::kLoRaPayloadSize))) {
        Serial.println("[LORA] send failed");
        return;
    }
    rf95.waitPacketSent();
}

void sendAck(uint8_t seq) {
    char line[16];
    snprintf(line, sizeof(line), "ACK,%u", static_cast<unsigned>(seq));
    Serial1.println(line);
    // Serial.print("[UART TX] ");
    // Serial.println(line);
}

// ---------------------------------------------------------------------------
#elif defined(LORA_BASE_STATION)
// ---------------------------------------------------------------------------
// BASE STATION MODE
//   - Listens for LoRa packets from any node
//   - Validates packet magic / type before forwarding
//   - Forwards to Jetson over UART (Serial1) with RSSI prepended:
//       [0xAA][0x55][len:32][rssi:i8][PktHeader:4][FullStatePayload:27][crc8]
//
// Build flags required: -D LORA_BASE_STATION=1
// No NODE_ID needed — the base station is not a sensor node.
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);   // USB — debug output
    Serial1.begin(115200);  // UART — Jetson link

    delay(1000);

    resetRadio();
    if (!rf95.init()) {
        Serial.println("[LORA] RH_RF95 init failed");
        while (1) { delay(100); }
    }
    if (!rf95.setFrequency(RFM95_FREQ)) {
        Serial.println("[LORA] setFrequency failed");
        while (1) { delay(100); }
    }
    rf95.setTxPower(13, false);

    Serial.println("[LORA] Base station ready");
}

void loop() {
    if (!rf95.available()) return;

    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len = RH_RF95_MAX_MESSAGE_LEN;

    if (!rf95.recv(buf, &len)) {
        Serial.println("[LORA] recv failed");
        return;
    }

    const int8_t rssi = static_cast<int8_t>(rf95.lastRssi());

    // Validate before forwarding — drop malformed packets silently
    BinaryPacket::PktHeader       hdr{};
    BinaryPacket::FullStatePayload payload{};
    if (!BinaryPacket::decodeFullState(buf, len, hdr, payload)) {
        Serial.print("[LORA] invalid packet, len=");
        Serial.println(len);
        return;
    }

    Serial.print("[LORA RX] node=");
    Serial.print(hdr.node_id);
    Serial.print(" seq=");
    Serial.print(hdr.seq);
    Serial.print(" rssi=");
    Serial.println(rssi);

    // Build and send UART frame to Jetson
    uint8_t frame[40];
    const size_t frame_len =
        BinaryPacket::encodeBaseFrame(rssi, buf, frame, sizeof(frame));

    if (frame_len > 0) {
        Serial1.write(frame, frame_len);
    } else {
        Serial.println("[UART] encodeBaseFrame failed");
    }
}

// ---------------------------------------------------------------------------
#else
  #error "Define either LORA_NODE=1 or LORA_BASE_STATION=1 in platformio.ini build_flags"
#endif
