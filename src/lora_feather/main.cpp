#include <Arduino.h>
#include <RH_RF95.h>
#include "shared/TelemetryPacket.h"
#include "shared/TelemetryCodec.h"

static char uartLine[192];
static size_t uartLineLen = 0;

TelemetryPacket lastTelemetry{};
bool hasTelemetry = false;
uint32_t lastRxMs = 0;

static constexpr int RFM95_CS  = 8;
static constexpr int RFM95_INT = 3;
static constexpr int RFM95_RST = 4;
static constexpr float RFM95_FREQ = 915.0;

RH_RF95 rf95(RFM95_CS, RFM95_INT);

void handleUartLine(const char* line);
void sendAck(uint32_t seq);
void sendErr(const char* reason);
void printTelemetryToUsb(const TelemetryPacket& pkt);
void forwardTelemetryOverLoRa(const TelemetryPacket& pkt);
void resetRadio();
void serviceLoRaRx();

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  delay(1000);

  resetRadio();
  if (!rf95.init()) {
    Serial.println("RH_RF95 init failed");
    while (1) { delay(100); }
  }

  Serial.println("RH_RF95 init OK");

  if (!rf95.setFrequency(RFM95_FREQ)) {
    Serial.println("setFrequency failed");
    while (1) { delay(100); }
  }

  Serial.println("Frequency set OK");

  rf95.setTxPower(13, false);   // safe starting point for Feather M0 RFM95
  Serial.println("LoRa peer test ready");

  Serial.println("Feather UART bridge alive");

  // Let ESP32 know we booted
  Serial1.println("ACK,BOOT");
}

void loop() {
  while (Serial1.available() > 0) {
    int c = Serial1.read();
    if (c < 0) continue;
    if (c == '\r') continue;

    if (c == '\n') {
      uartLine[uartLineLen] = '\0';

      if (uartLineLen > 0) {
        // all sensor data is sent over uart
        handleUartLine(uartLine);
      }

      uartLineLen = 0;
      continue;
    }

    if (uartLineLen + 1 < sizeof(uartLine)) {
      uartLine[uartLineLen++] = (char)c;
    } else {
      uartLineLen = 0;
      sendErr("overflow");
    }
  }
  serviceLoRaRx();
}

// uart helper functions
void handleUartLine(const char* line) {
  lastRxMs = millis();

  Serial.print("[UART RX] ");
  Serial.println(line);

  if (strcmp(line, "HELLO,ESP32") == 0) {
    Serial.println("Boot hello from ESP32");
    Serial1.println("ACK,BOOT");
    return;
  }

  TelemetryPacket pkt{};
  if (TelemetryCodec::parse(line, pkt)) {
    hasTelemetry = true;
    lastTelemetry = pkt;

    //printTelemetryToUsb(pkt);
    sendAck(pkt.seq);

    // send uart data over lora
    // FIX: lora is much slower than uart. Need to cache or shrink packets somehow.
    forwardTelemetryOverLoRa(pkt);
    return;
  }

  sendErr("unknown_packet");
}
void sendAck(uint32_t seq) {
  char line[32];
  snprintf(line, sizeof(line), "ACK,%lu", (unsigned long)seq);

  Serial.print("[UART TX] ");
  Serial.println(line);

  Serial1.println(line);
}

void sendErr(const char* reason) {
  char line[64];
  snprintf(line, sizeof(line), "ERR,%s", reason);

  Serial.print("[UART TX] ");
  Serial.println(line);

  Serial1.println(line);
}

void printTelemetryToUsb(const TelemetryPacket& pkt) {
  Serial.println("Telemetry parsed:");
  Serial.print("  seq: ");
  Serial.println(pkt.seq);

  Serial.print("  uptimeMs: ");
  Serial.println(pkt.uptimeMs);

  Serial.print("  sensorFlags: ");
  Serial.println(pkt.sensorFlags);

  if (pkt.sensorFlags & (1 << 0)) {
    Serial.print("  flameDetected: ");
    Serial.println(pkt.flameDetected ? "YES" : "NO");
  }

  if (pkt.sensorFlags & (1 << 1)) {
    Serial.print("  windMps: ");
    Serial.println(pkt.windMps, 3);
  }

  if (pkt.sensorFlags & (1 << 2)) {
    Serial.print("  tempC: ");
    Serial.println(pkt.tempC, 2);
    Serial.print("  humidityPct: ");
    Serial.println(pkt.humidityPct, 2);
  }

  if (pkt.sensorFlags & (1 << 3)) {
    Serial.print("  lidarCm: ");
    Serial.println(pkt.lidarCm);
  }

  if (pkt.sensorFlags & (1 << 4)) {
    Serial.print("  lat: ");
    Serial.println(pkt.lat, 6);
    Serial.print("  lon: ");
    Serial.println(pkt.lon, 6);
  }

  Serial.println();
}

// lora helper methods
void forwardTelemetryOverLoRa(const TelemetryPacket& pkt) {
  char line[192];
  if (!TelemetryCodec::encode(pkt, line, sizeof(line))) {
    Serial.println("[LORA] encode failed");
    return;
  }

  const uint8_t len = (uint8_t)strlen(line);
  if (!rf95.send((const uint8_t*)line, len)) {
    Serial.println("[LORA] send failed");
    return;
  }

  rf95.waitPacketSent();

  Serial.print("[LORA TX] ");
  Serial.println(line);
}

void resetRadio() {
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
}

void serviceLoRaRx() {
  //FIX: no proper lora ack quite yet. would like to implement a lite-TCP using heartbeats and a checksum.
  if (!rf95.available()) {
    return;
  }

  uint8_t buf[RH_RF95_MAX_MESSAGE_LEN + 1];
  uint8_t len = RH_RF95_MAX_MESSAGE_LEN;

  if (!rf95.recv(buf, &len)) {
    Serial.println("[LORA RX] recv failed");
    return;
  }

  buf[len] = '\0';

  Serial.print("[LORA RX] ");
  Serial.println((char*)buf);

  Serial.print("[LORA RSSI] ");
  Serial.println(rf95.lastRssi());

  TelemetryPacket pkt{};
  if (TelemetryCodec::parse((const char*)buf, pkt)) {
    Serial.println("[LORA RX] parsed packet:");
    printTelemetryToUsb(pkt);
  } else {
    Serial.println("[LORA RX] parse failed");
  }
}

