#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>

// Adafruit Feather M0 with built-in RFM95 LoRa radio.
#define RFM95_CS  8
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

// SmartFires packet assumptions:
// PktHeader is 4 bytes:
//   magic, pkt_type, node_id, seq
// with magic = 0xA5.
//
// FULL_STATE packet assumptions:
//   bytes 0..3:   magic, pkt_type, node_id, seq
//   bytes 4..7:   session_time_ms uint32 little-endian
//   bytes 8..11:  uptime_ms uint32 little-endian
//
// If your actual packet format differs, only decodeSmartFiresPayload()
// needs edits.
static constexpr uint8_t SMARTFIRES_PKT_MAGIC = 0xA5;

// static const char *packetTypeName(uint8_t pktType) {
//   switch (pktType) {
//   case 0x01:
//     return "FULL_STATE";
//   case 0x02:
//     return "HEARTBEAT";
//   case 0x03:
//     return "TIME_SYNC";
//   default:
//     return "UNKNOWN";
//   }
// }
static const char *packetTypeName(uint8_t pktType) {
  switch (pktType) {
  case 0x01:
    return "FULL_STATE";
  case 0x02:
    return "HEARTBEAT";
  case 0x03:
    return "TIME_SYNC";
  case 0x04:
    return "BUNDLE";
  case 0x05:
    return "STATUS";
  case 0x06:
    return "AWAKEN";
  case 0x07:
    return "ACK_SUMMARY";
  default:
    return "UNKNOWN";
  }
}

static uint32_t readLeU32(const uint8_t *buf) {
  return static_cast<uint32_t>(buf[0]) |
         (static_cast<uint32_t>(buf[1]) << 8) |
         (static_cast<uint32_t>(buf[2]) << 16) |
         (static_cast<uint32_t>(buf[3]) << 24);
}

static void printHexByte(uint8_t b) {
  if (b < 0x10) {
    Serial.print('0');
  }
  Serial.print(b, HEX);
}

static void printHexBuffer(const uint8_t *buf, uint8_t len) {
  for (uint8_t i = 0; i < len; ++i) {
    printHexByte(buf[i]);
    if (i + 1 < len) {
      Serial.print(' ');
    }
  }
}

static void printAsciiPreview(const uint8_t *buf, uint8_t len) {
  for (uint8_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(buf[i]);
    if (c >= 32 && c <= 126) {
      Serial.print(c);
    } else {
      Serial.print('.');
    }
  }
}

static void decodeSmartFiresPayload(const uint8_t *buf, uint8_t len) {
  if (len < 4) {
    Serial.print(" decode=too_short");
    return;
  }

  const uint8_t magic = buf[0];
  const uint8_t pktType = buf[1];
  const uint8_t nodeId = buf[2];
  const uint8_t seq = buf[3];

  if (magic != SMARTFIRES_PKT_MAGIC) {
    Serial.print(" decode=not_smartfires");
    Serial.print(" first_byte=0x");
    printHexByte(magic);
    return;
  }

  Serial.print(" decode=smartfires");

  Serial.print(" type=");
  Serial.print(packetTypeName(pktType));
  Serial.print("(");
  Serial.print(pktType);
  Serial.print(")");

  Serial.print(" node=");
  Serial.print(nodeId);

  Serial.print(" seq=");
  Serial.print(seq);

  Serial.print(" payload_len=");
  Serial.print(len);

  // Decode timing fields from FULL_STATE packets.
  // This helps the Python dashboard classify packets into TDMA frames/slots
  // using the SmartFires network clock instead of only the sniffer millis().
  // if (pktType == 0x01 && len >= 12) {
  //   const uint32_t sessionTimeMs = readLeU32(&buf[4]);
  //   const uint32_t uptimeMs = readLeU32(&buf[8]);
  //
  //   Serial.print(" session_time_ms=");
  //   Serial.print(sessionTimeMs);
  //
  //   Serial.print(" uptime_ms=");
  //   Serial.print(uptimeMs);
  // }
  // Decode session time from FULL_STATE and BUNDLE packets.
  // Current refactor/only-feather format:
  //   bytes 0..3: magic, pkt_type, node_id, seq
  //   bytes 4..7: FullStatePayload.session_time uint32 little-endian
  if ((pktType == 0x01 || pktType == 0x04) && len >= 8) {
    const uint32_t sessionTimeMs = readLeU32(&buf[4]);

    Serial.print(" session_time_ms=");
    Serial.print(sessionTimeMs);
  }
}

static void printRadioHeadHeaders() {
  Serial.print(" rh_to=");
  Serial.print(rf95.headerTo());

  Serial.print(" rh_from=");
  Serial.print(rf95.headerFrom());

  Serial.print(" rh_id=");
  Serial.print(rf95.headerId());

  Serial.print(" rh_flags=0x");
  printHexByte(rf95.headerFlags());
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

  Serial.println();
  Serial.println("SmartFires passive LoRa sniffer starting...");
  Serial.println("Mode: receive-only / no TDMA / no ACK / no transmit");

  resetRadioModule();

  if (!rf95.init()) {
    Serial.println("ERROR: RFM95 init failed");
    while (true) {
      delay(1000);
    }
  }

  if (!rf95.setFrequency(SNIFFER_RF95_FREQ_MHZ)) {
    Serial.println("ERROR: setFrequency failed");
    while (true) {
      delay(1000);
    }
  }

  // Promiscuous mode accepts packets regardless of RadioHead destination header.
  // This makes the device useful as a passive monitor.
  rf95.setPromiscuous(true);

  // Keep this matched to the real network if your driver changes these.
  // RadioHead defaults are fine only if your main firmware also uses defaults.
  //
  // If your TDMA driver sets modem config explicitly, mirror it here.
  // Example:
  // rf95.setModemConfig(RH_RF95::Bw125Cr45Sf128);

  Serial.print("Frequency MHz: ");
  Serial.println(SNIFFER_RF95_FREQ_MHZ, 3);

  Serial.println("Listening...");
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
    Serial.print("RX_FAIL");
    Serial.print(" t_ms=");
    Serial.print(now);
    Serial.print(" bad_count=");
    Serial.println(badCount);
    return;
  }

  ++packetCount;

  const uint32_t dt = (lastPacketMs == 0) ? 0 : (now - lastPacketMs);
  lastPacketMs = now;

  Serial.print("RX");
  Serial.print(" count=");
  Serial.print(packetCount);

  Serial.print(" t_ms=");
  Serial.print(now);

  Serial.print(" dt_ms=");
  Serial.print(dt);

  Serial.print(" len=");
  Serial.print(len);

  Serial.print(" rssi=");
  Serial.print(rf95.lastRssi());

#if defined(RH_RF95_REG_PKT_SNR_VALUE)
  Serial.print(" snr=");
  Serial.print(rf95.lastSNR());
#endif

  printRadioHeadHeaders();

  decodeSmartFiresPayload(buf, len);

  Serial.print(" hex=[");
  printHexBuffer(buf, len);
  Serial.print("]");

  Serial.print(" ascii=\"");
  printAsciiPreview(buf, len);
  Serial.print("\"");

  Serial.println();
}
