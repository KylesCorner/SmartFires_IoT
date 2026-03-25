#include <Arduino.h>   // Core Arduino types/functions: setup(), loop(), pinMode(), millis(), etc.
#include <SPI.h>       // SPI bus driver used by the RFM95 LoRa radio.
#include <RH_RF95.h>   // RadioHead driver for SX1276/77/78/79 class radios (RFM95 module).

// Pin mapping for the built-in RFM95 radio on Adafruit Feather M0 LoRa boards.
// These constants tell the driver how to talk to the radio chip.
static constexpr uint8_t PIN_RFM95_CS = 8;   // SPI chip-select (NSS): selects the radio on SPI bus.
static constexpr uint8_t PIN_RFM95_RST = 4;  // Hardware reset line to force radio into known state.
static constexpr uint8_t PIN_RFM95_INT = 3;  // DIO0 interrupt pin: signals TX done / RX done events.

// RF center frequency in MHz.
// Use 915.0 in North America ISM band for 915 MHz radios.
static constexpr float LORA_FREQ_MHZ = 915.0f;

// Node identifier embedded in payload.
// This lets a receiver distinguish packets when multiple nodes are active.
static constexpr uint8_t NODE_ID = 1;

// Global radio driver instance configured with CS and interrupt pin.
RH_RF95 rf95(PIN_RFM95_CS, PIN_RFM95_INT);

// Monotonic packet sequence number for packet loss and ordering checks.
uint32_t txSeq = 0;

// Timestamp (millis) when we sent the previous packet.
// Used to send at a fixed interval instead of spamming continuously.
uint32_t lastTxMs = 0;

// Initializes and configures the radio.
// Returns true on success, false if any mandatory step fails.
bool initRadio() {
  // Configure reset pin as output so firmware can pulse hardware reset.
  pinMode(PIN_RFM95_RST, OUTPUT);

  // Keep reset de-asserted (HIGH) briefly.
  digitalWrite(PIN_RFM95_RST, HIGH);
  delay(10);

  // Assert reset (LOW) to reset the radio chip.
  digitalWrite(PIN_RFM95_RST, LOW);
  delay(10);

  // Release reset (HIGH) so chip boots into normal operation.
  digitalWrite(PIN_RFM95_RST, HIGH);
  delay(10);

  // Initialize radio driver and verify SPI communication with chip.
  if (!rf95.init()) {
    return false;
  }

  // Program radio operating frequency.
  // If this fails, TX and RX will never match channels.
  if (!rf95.setFrequency(LORA_FREQ_MHZ)) {
    return false;
  }

  // Set transmit power in dBm.
  // 20 dBm is max-ish for many modules; second arg false means use PA_BOOST mode selection per driver defaults.
  rf95.setTxPower(20, false);

  // Signal success to caller.
  return true;
}

void setup() {
  // Start USB serial for diagnostics and bring-up logs.
  Serial.begin(115200);

  // On native USB boards, wait until terminal opens so startup logs are not missed.
  while (!Serial) {
    delay(1);
  }

  // Human-readable boot marker in serial monitor.
  Serial.println("\nSmartFires LoRa bring-up");

  // Initialize radio; halt if radio is not detected/configured.
  if (!initRadio()) {
    Serial.println("Radio init failed. Check board type, antenna, and wiring.");

    // Fail-fast loop: keeps error visible and prevents running invalid logic.
    while (true) {
      delay(1000);
    }
  }

  // Compile-time role selection.
  // Exactly one role should be defined by PlatformIO build flags.
#if defined(LORA_ROLE_TX)
  Serial.println("Role: TX");
#elif defined(LORA_ROLE_RX)
  Serial.println("Role: RX");
#else
  Serial.println("No role selected. Build with -D LORA_ROLE_TX or -D LORA_ROLE_RX.");
  while (true) {
    delay(1000);
  }
#endif
}

void loop() {
  // Transmitter role: send one packet every 1000 ms.
#if defined(LORA_ROLE_TX)
  // Current uptime in milliseconds since boot.
  const uint32_t now = millis();

  // Non-blocking interval check.
  // This avoids using delay(1000), so code stays responsive and extensible.
  if (now - lastTxMs >= 1000) {
    // Record send time immediately to hold steady cadence.
    lastTxMs = now;

    // Static stack buffer for ASCII payload.
    // Increased slightly to fit fake telemetry fields for testing.
    char packet[128];

    // Build deterministic fake telemetry values for end-to-end parser tests.
    const uint32_t seq = txSeq++;
    const int tempCx10 = 220 + static_cast<int>((seq * 13UL + now / 1000UL) % 90UL);  // 22.0C to 30.9C
    const int humPct = 35 + static_cast<int>((seq * 11UL + now / 2000UL) % 41UL);      // 35% to 75%
    const int smokePpm = 8 + static_cast<int>((seq * 17UL + now / 1500UL) % 90UL);      // 8 to 97 ppm
    const int battMv = 4020 - static_cast<int>((seq * 3UL) % 220UL);                    // 3800 to 4020 mV
    const uint8_t fireFlag = (tempCx10 >= 285 || smokePpm >= 70) ? 1 : 0;

    // Format payload with base metadata plus fake telemetry.
    const int len = snprintf(packet, sizeof(packet),
                             "node=%u,seq=%lu,ts=%lu,temp_c=%d.%d,hum_pct=%d,smoke_ppm=%d,batt_mv=%d,fire=%u",
                             NODE_ID,
                             static_cast<unsigned long>(seq),
                             static_cast<unsigned long>(now),
                             tempCx10 / 10,
                             tempCx10 % 10,
                             humPct,
                             smokePpm,
                             battMv,
                             fireFlag);

    // Only send if formatting succeeded without truncation.
    if (len > 0 && len < static_cast<int>(sizeof(packet))) {
      // Cast text buffer to byte buffer for radio send API.
      rf95.send(reinterpret_cast<const uint8_t*>(packet), static_cast<uint8_t>(len));

      // Block until on-air transmission completes.
      // This keeps serial logs aligned with actual sends.
      rf95.waitPacketSent();

      // Print sent payload to serial so user can correlate TX and RX logs.
      Serial.print("TX -> ");
      Serial.println(packet);
    }
  }

  // Receiver role: poll for inbound packets and print decoded ASCII + RSSI.
#elif defined(LORA_ROLE_RX)
  // Check if radio has a packet waiting in RX buffer.
  if (rf95.available()) {
    // Allocate maximum message buffer supported by driver.
    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];

    // len is both input capacity and output actual packet length.
    uint8_t len = sizeof(buf);

    // Attempt packet receive; returns true only for valid packet extraction.
    if (rf95.recv(buf, &len)) {
      // Prefix so received lines are easy to filter in serial logs.
      Serial.print("RX <- ");

      // Print payload byte-by-byte as characters.
      for (uint8_t i = 0; i < len; i++) {
        Serial.print(static_cast<char>(buf[i]));
      }

      // Append received signal strength indicator for link quality checks.
      Serial.print(" | RSSI=");
      Serial.println(rf95.lastRssi());
    }
  }
#endif
}