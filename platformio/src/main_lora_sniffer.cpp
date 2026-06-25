// ---
// description: Passive LoRa sniffer firmware entrypoint — promiscuous-mode RFM95 receive loop emitting NDJSON packet records over USB serial, no TDMA/ACK/transmit.
// role: entrypoint
// ---
#include <Arduino.h>
#include <RH_RF95.h>
#include <SPI.h>

// Adafruit Feather M0 with built-in RFM95 LoRa radio.
#define RFM95_CS 8
#define RFM95_INT 3
#define RFM95_RST 4

// Must match your deployed SmartFires LoRa settings.
#ifndef SNIFFER_RF95_FREQ_MHZ
#define SNIFFER_RF95_FREQ_MHZ 915.0
#endif

#ifndef SNIFFER_MAX_PACKET_LEN
#define SNIFFER_MAX_PACKET_LEN RH_RF95_MAX_MESSAGE_LEN
#endif

RH_RF95 rf95(RFM95_CS, RFM95_INT);

static uint32_t packetCount = 0;
static uint32_t badCount = 0;
static uint32_t lastPacketMs = 0;

static void printHexByte(uint8_t b) {
  if (b < 0x10) {
    Serial.print('0');
  }
  Serial.print(b, HEX);
}

static void printHexBufferCompact(const uint8_t *buf, uint8_t len) {
  for (uint8_t i = 0; i < len; ++i) {
    printHexByte(buf[i]);
  }
}

static void printJsonString(const char *s) {
  Serial.print('"');
  while (*s) {
    const char c = *s++;
    switch (c) {
    case '"':
      Serial.print("\\\"");
      break;
    case '\\':
      Serial.print("\\\\");
      break;
    case '\n':
      Serial.print("\\n");
      break;
    case '\r':
      Serial.print("\\r");
      break;
    case '\t':
      Serial.print("\\t");
      break;
    default:
      if (static_cast<uint8_t>(c) < 0x20) {
        Serial.print("\\u00");
        printHexByte(static_cast<uint8_t>(c));
      } else {
        Serial.print(c);
      }
      break;
    }
  }
  Serial.print('"');
}

static void emitStatus(const char *message) {
  Serial.print("{\"event\":\"status\",\"t_ms\":");
  Serial.print(millis());
  Serial.print(",\"message\":");
  printJsonString(message);
  Serial.println("}");
}

static void emitError(const char *message) {
  Serial.print("{\"event\":\"error\",\"t_ms\":");
  Serial.print(millis());
  Serial.print(",\"message\":");
  printJsonString(message);
  Serial.println("}");
}

static void resetRadioModule() {
  pinMode(RFM95_RST, OUTPUT);

  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  digitalWrite(RFM95_RST, LOW);
  delay(10);

  digitalWrite(RFM95_RST, HIGH);
  delay(10);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  emitStatus("SmartFires passive LoRa sniffer starting");
  emitStatus("Mode: receive-only/no-tdma/no-ack/no-transmit");

  resetRadioModule();

  if (!rf95.init()) {
    emitError("RFM95 init failed");
    while (true) {
      delay(1000);
    }
  }

  if (!rf95.setFrequency(SNIFFER_RF95_FREQ_MHZ)) {
    emitError("setFrequency failed");
    while (true) {
      delay(1000);
    }
  }

  // Promiscuous mode accepts packets regardless of RadioHead destination
  // header.
  rf95.setPromiscuous(true);

  // Keep this matched to the real network if your main firmware changes it.
  // Example:
  // rf95.setModemConfig(RH_RF95::Bw125Cr45Sf128);

  Serial.print("{\"event\":\"config\",\"t_ms\":");
  Serial.print(millis());
  Serial.print(",\"rf95_freq_mhz\":");
  Serial.print(SNIFFER_RF95_FREQ_MHZ, 3);
  Serial.print(",\"max_packet_len\":");
  Serial.print(SNIFFER_MAX_PACKET_LEN);
  Serial.println("}");

  emitStatus("Listening");
}

void loop() {
  if (!rf95.available()) {
    return;
  }

  uint8_t buf[SNIFFER_MAX_PACKET_LEN];
  uint8_t len = sizeof(buf);

  const uint32_t now = millis();

  if (!rf95.recv(buf, &len)) {
    ++badCount;

    Serial.print("{\"event\":\"rx_fail\",\"t_ms\":");
    Serial.print(now);
    Serial.print(",\"bad_count\":");
    Serial.print(badCount);
    Serial.println("}");

    return;
  }

  ++packetCount;

  const uint32_t dt = (lastPacketMs == 0) ? 0 : (now - lastPacketMs);
  lastPacketMs = now;

  // NDJSON: one complete JSON object per line.
  //
  // Firmware intentionally does no SmartFires packet-name filtering.
  // Python owns all packet decoding, packet naming, TDMA classification,
  // and dashboard filtering.
  Serial.print("{\"event\":\"rx\"");

  Serial.print(",\"count\":");
  Serial.print(packetCount);

  Serial.print(",\"t_ms\":");
  Serial.print(now);

  Serial.print(",\"dt_ms\":");
  Serial.print(dt);

  Serial.print(",\"len\":");
  Serial.print(len);

  Serial.print(",\"rssi\":");
  Serial.print(rf95.lastRssi());

  Serial.print(",\"snr\":");
#if defined(RH_RF95_REG_PKT_SNR_VALUE)
  Serial.print(rf95.lastSNR());
#else
  Serial.print("null");
#endif

  Serial.print(",\"rh_to\":");
  Serial.print(rf95.headerTo());

  Serial.print(",\"rh_from\":");
  Serial.print(rf95.headerFrom());

  Serial.print(",\"rh_id\":");
  Serial.print(rf95.headerId());

  Serial.print(",\"rh_flags\":");
  Serial.print(rf95.headerFlags());

  Serial.print(",\"payload_hex\":\"");
  printHexBufferCompact(buf, len);
  Serial.print("\"");

  Serial.println("}");
}
