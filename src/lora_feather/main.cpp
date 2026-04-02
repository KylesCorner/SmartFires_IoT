#include <Arduino.h>
#include "shared/TelemetryPacket.h"
#include "shared/TelemetryCodec.h"

static char uartLine[192];
static size_t uartLineLen = 0;

TelemetryPacket lastTelemetry{};
bool hasTelemetry = false;
uint32_t lastAckedSeq = 0;
uint32_t lastRxMs = 0;

void handleUartLine(const char* line);
void sendAck(uint32_t seq);
void sendErr(const char* reason);
void printTelemetryToUsb(const TelemetryPacket& pkt);
void forwardTelemetryOverLoRa(const TelemetryPacket& pkt);

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  delay(1000);

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

  // Future radio servicing can go here
  // Example:
  // radio.update();
}
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
    lastAckedSeq = pkt.seq;

    printTelemetryToUsb(pkt);
    sendAck(pkt.seq);

    // Placeholder for actual radio forwarding
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
void forwardTelemetryOverLoRa(const TelemetryPacket& pkt) {
  // For now, just show that this is where radio send would happen.
  Serial.print("[LORA] Forwarding telemetry seq ");
  Serial.println(pkt.seq);

  // Later, you can either:
  // 1. re-encode the same TEL line and send it over radio, or
  // 2. pack into a smaller binary frame for radio efficiency.
}
