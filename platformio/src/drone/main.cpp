#include <Arduino.h>

#include "FlameSensor.h"
#include "ISensor.h"
// #include "PassiveBuzzer.h"
#include "Icm20948Imu.h"
#include "PinMapping.h"
// #include "MicroServo.h"
#include "LidarLiteV3.h"
#include "MatrixKeypadSensor.h"
#include "OledDisplay.h"
#include "Pa1010dGpsSensor.h"
#include "Sht31Sensor.h"
#include "USBCDC.h"
#include "WindSensorRevC.h"
#include "shared/BinaryPacket.h"
#include "shared/TelemetryPacket.h"
#include "shared/UartLoRaBridge.h"

#ifndef NODE_ID
#error "NODE_ID must be set in platformio.ini build_flags (e.g. -D NODE_ID=1)"
#endif

// Sensors
WindSensorRevC wind(PIN_WIND_RV, PIN_WIND_TMP);
Icm20948Imu imu(1);
FlameSensor flame(PIN_FLAME_AO, PIN_FLAME_DO);
Sht31Sensor sht31(Sht31Sensor::kAlternateAddress);
Pa1010dGpsSensor gps(Wire);
LidarLiteV3 lidar;
MatrixKeypadSensor keypad(KEYPAD_ROWS, KEYPAD_COLS, KEYPAD_MAP, "Drone Keypad");

// Actuators
// PassiveBuzzer buzzer(PIN_BUZZER); // choose a PWM-capable pin (not required
// for tone()) MicroServo servo(PIN_SERVO); // choose a PWM-capable pin
OledDisplay oled(OledDisplay::Controller::SH1106, 0x3C, "OLED Display");

// Polymorphic registry
ISensor *sensors[] = {&flame, &imu, &sht31, &gps, &wind, &lidar, &keypad};
constexpr size_t kNumSensors = sizeof(sensors) / sizeof(sensors[0]);

IActuator *actuators[] = {&oled};
constexpr size_t kNumActuators = sizeof(actuators) / sizeof(actuators[0]);

// Basic scheduling
uint32_t lastPrintMs = 0;

// page/oled setup
enum OledPage : uint8_t {
  PAGE_ENV = 0,
  PAGE_GPS,
  PAGE_IMU,
  PAGE_LIDAR,
  PAGE_UART,
  PAGE_LORA,
  PAGE_COUNT
};

uint8_t currentPage = PAGE_ENV;
bool sensingEnabled = true;
bool oledNeedsRefresh = true;
void handleKeypad();
void renderOled();

// Networking Setup
HardwareSerial LoRaSerial(1);
UartLoRaBridge loraBridge(LoRaSerial, PIN_LORA_RX, PIN_LORA_TX, 115200);

uint32_t lastSendMs = 0;
uint32_t seq = 0;
uint32_t lastSentSeq = 0;
uint32_t lastUARTAckedSeq = 0;
bool waitingForUARTAck = false;
uint32_t lastSendTimeMs = 0;
enum TelemetryFlags : uint8_t {
  TF_FLAME = 1 << 0,
  TF_WIND = 1 << 1,
  TF_SHT31 = 1 << 2,
  TF_LIDAR = 1 << 3,
  TF_GPS = 1 << 4,
  TF_IMU = 1 << 5
};

void sendTelemetry(uint32_t seq);
TelemetryPacket buildTelemetryPacket(uint32_t seq);
void handleBridgeLine(const char *line);
void serviceBridge();

void setup() {
  Serial.begin(115200);
  // Serial1.begin(115200);
  delay(200);
  Wire.begin();

  for (size_t i = 0; i < kNumSensors; i++) {
    bool ok = sensors[i]->begin();
    Serial.print("Begin ");
    Serial.print(sensors[i]->name());
    Serial.print(": ");
    Serial.println(ok ? "OK" : "FAIL");
  }
  for (size_t i = 0; i < kNumActuators; i++) {
    bool ok = actuators[i]->begin();
    Serial.print("Begin ");
    Serial.print(actuators[i]->name());
    Serial.print(": ");
    Serial.println(ok ? "OK" : "FAIL");
  }

  loraBridge.begin();

  // Quick startup chirp
  // buzzer.beep(2000, 20);
}
void loop() {
  // Pull in any UART bytes and process complete lines
  loraBridge.update();
  serviceBridge();

  // Sample everything frequently (each sensor can self-throttle internally)
  if (sensingEnabled) {
    for (size_t i = 0; i < kNumSensors; i++) {
      sensors[i]->sample();
    }
  } else {
    keypad.sample();
  }

  // Update actuators
  for (size_t i = 0; i < kNumActuators; i++) {
    actuators[i]->update();
  }

  handleKeypad();

  const uint32_t now = millis();

  // Send telemetry at a controlled cadence
  if (sensingEnabled && (now - lastSendMs >= UartLoRaBridge::kTelemetryPeriodMs)) {
    lastSendMs = now;

    // Optional: only allow one in-flight packet at a time
    if (!waitingForUARTAck) {
      ++seq;
      sendTelemetry(seq);
      lastSentSeq = seq;
      lastSendTimeMs = now;
      waitingForUARTAck = true;
    } else {
      Serial.println("[UART] still waiting for ACK, skipping send");
    }
  }

  // ACK timeout handling
  if (waitingForUARTAck &&
      (now - lastSendTimeMs >= UartLoRaBridge::kAckTimeoutMs)) {
    Serial.print("[UART] ACK timeout for seq ");
    Serial.println(lastSentSeq);
    waitingForUARTAck = false;
  }

  if (oled.healthy()) {
    renderOled();
  }
}

void handleKeypad() {
  if (!keypad.keyAvailable())
    return;

  const char key = keypad.consumeKey();
  if (key == '\0')
    return;

  Serial.print("[KEYPAD] pressed: ");
  Serial.println(key);

  switch (key) {
  case KEYPAD_PREV_PAGE:
    currentPage = (currentPage == 0) ? (PAGE_COUNT - 1) : (currentPage - 1);
    oledNeedsRefresh = true;
    break;

  case KEYPAD_NEXT_PAGE:
    currentPage = (currentPage + 1) % PAGE_COUNT;
    oledNeedsRefresh = true;
    break;

  case '#': // toggle sensing
    sensingEnabled = !sensingEnabled;
    Serial.print("[KEYPAD] sensing ");
    Serial.println(sensingEnabled ? "ENABLED" : "DISABLED");
    oledNeedsRefresh = true;
    break;

  case '*': // home page
    currentPage = PAGE_ENV;
    oledNeedsRefresh = true;
    break;

  default:
    Serial.printf("[KEYPAD] key: %c reserved\n",key);
    break;
  }
}
void renderOled() {
  switch (currentPage) {
  case PAGE_ENV:
    oled.printLine(0, sensingEnabled ? "Environment" : "Environment [PAUSED]");
    if (wind.hasReading()) {
      oled.printfLine(1, "Wind Speed:%.3f", wind.windMps());
    } else {
      oled.printLine(1, sensingEnabled ? "Wind: Warming up" : "Wind: paused");
    }

    if (sht31.hasReading()) {
      oled.printfLine(2, "T:%.1fF %.1fC H:%1.f%%", sht31.temperatureF(),
                      sht31.temperatureC(), sht31.humidityPct());
    } else {
      oled.printLine(2, sensingEnabled ? "T/H:no reading" : "T/H: paused");
    }

    if (flame.hasReading()) {
      oled.printfLine(3, "Flame:%d", flame.analogRaw());
    } else {
      oled.printLine(3, sensingEnabled ? "No flame reading" : "Flame: paused");
    }
    break;

  case PAGE_GPS:
    oled.printLine(0, sensingEnabled ? "GPS" : "GPS [PAUSED]");

    if (!gps.hasReading()) {
      oled.printLine(1, sensingEnabled ? "No data" : "Paused");
      oled.printLine(2, "");
      oled.printLine(3, "");
    } else if (!gps.hasFix()) {
      oled.printfLine(1, "No fix S:%u", gps.satellites());
      oled.printfLine(2, "Age:%lu ms", (unsigned long)gps.sentenceAgeMs());
      oled.printLine(3, "Go outside");
    } else {
      oled.printfLine(1, "S:%u Alt:%.0fm", gps.satellites(),
                      gps.altitudeMeters());
      oled.printfLine(2, "Lat:%.4f", gps.latitudeDegrees());
      oled.printfLine(3, "Lon:%.4f", gps.longitudeDegrees());
    }
    break;

  case PAGE_IMU:
    oled.printLine(0, sensingEnabled ? "IMU" : "IMU [PAUSED]");

    if (imu.hasReading()) {
      oled.printfLine(1, "A:%.1f %.1f %.1f", imu.ax_mg(), imu.ay_mg(),
                      imu.az_mg());
      oled.printfLine(2, "G:%.1f %.1f %.1f", imu.gx_dps(), imu.gy_dps(),
                      imu.gz_dps());
      oled.printfLine(3, "M:%.1f %.1f %.1f", imu.mx_uT(), imu.my_uT(),
                      imu.mz_uT());
    } else {
      oled.printLine(1, sensingEnabled ? "No IMU reading" : "Paused");
      oled.printLine(2, "");
      oled.printLine(3, "");
    }
    break;

  case PAGE_UART:
    oled.printLine(0, "UART Connection");
    oled.printfLine(1, "Sent:%lu Ack:%lu", (unsigned long)lastSentSeq,
                    (unsigned long)lastUARTAckedSeq);
    oled.printfLine(2, "WaitAck:%s", waitingForUARTAck ? "YES" : "NO");
    oled.printfLine(3, "UART:%s",
                    loraBridge.hasError() ? loraBridge.lastError() : "OK");
    break;

  case PAGE_LIDAR:
    oled.printLine(0, sensingEnabled ? "LIDAR" : "LIDAR [PAUSED]");
    oled.printLine(2, "");
    oled.printLine(3, "");
    if (lidar.hasReading()) {
      oled.printfLine(1, "Dist: %d cm", lidar.distanceCm());
    } else {
      oled.printLine(1, sensingEnabled ? "No readings" : "Paused");
    }
    break;

  case PAGE_LORA: {
    oled.printLine(0, "LoRa Link");

    const bool bootSeen = loraBridge.bootSeen();
    const bool linkAlive =
        loraBridge.bootSeen() &&
        ((millis() - lastSendTimeMs) < 3000 || waitingForUARTAck == false);

    oled.printfLine(1, "B:%s L:%s", bootSeen ? "Y" : "N",
                    linkAlive ? "OK" : "WAIT");

    oled.printfLine(2, "S:%lu A:%lu", (unsigned long)lastSentSeq,
                    (unsigned long)lastUARTAckedSeq);

    if (loraBridge.hasRx()) {
      char rxPreview[22];
      strncpy(rxPreview, loraBridge.lastRx(), sizeof(rxPreview) - 1);
      rxPreview[sizeof(rxPreview) - 1] = '\0';
      oled.printfLine(3, "RX:%s", rxPreview);
    } else if (waitingForUARTAck) {
      oled.printLine(3, "RX: waiting ack");
    } else {
      oled.printLine(3, "RX: none");
    }
    break;
  }

  default:
    oled.printLine(0, "Empty Page");
    oled.printLine(1, "");
    oled.printLine(2, "");
    oled.printLine(3, "");
    break;
  }
}

TelemetryPacket buildTelemetryPacket(uint32_t seq) {
  TelemetryPacket p{};
  p.seq = seq;
  p.uptimeMs = millis();

  if (flame.hasReading()) {
    p.sensorFlags |= TF_FLAME;
    p.flameDetected = flame.detected();
  }

  if (wind.hasReading()) {
    p.sensorFlags |= TF_WIND;
    p.windMps = wind.windMps();
  }

  if (sht31.hasReading()) {
    p.sensorFlags |= TF_SHT31;
    p.tempC = sht31.temperatureC();
    p.humidityPct = sht31.humidityPct();
  }

  if (lidar.hasReading()) {
    p.sensorFlags |= TF_LIDAR;
    p.lidarCm = lidar.distanceCm();
  }

  if (gps.hasReading() && gps.hasFix()) {
    p.sensorFlags |= TF_GPS;
    p.lat = gps.latitudeDegrees();
    p.lon = gps.longitudeDegrees();
  }

  return p;
}

void sendTelemetry(uint32_t seq) {
  TelemetryPacket pkt = buildTelemetryPacket(seq);

  BinaryPacket::FullStatePayload payload{};
  payload.session_time = pkt.uptimeMs; // local millis until TIME_SYNC is live
  payload.uptime_ms = pkt.uptimeMs;
  payload.sensor_flags = pkt.sensorFlags;
  payload.flame = pkt.flameDetected ? 1u : 0u;
  payload.wind_cms = static_cast<uint16_t>(pkt.windMps * 100.0f + 0.5f);
  payload.temp_cdegc = static_cast<int16_t>(pkt.tempC * 100.0f);
  payload.humidity_cpct =
      static_cast<uint16_t>(pkt.humidityPct * 100.0f + 0.5f);
  payload.lidar_cm = static_cast<uint16_t>(pkt.lidarCm > 0 ? pkt.lidarCm : 0);
  payload.lat_e7 = static_cast<int32_t>(pkt.lat * 1e7);
  payload.lon_e7 = static_cast<int32_t>(pkt.lon * 1e7);

  uint8_t frame[40];
  const size_t len = BinaryPacket::encodeFullStateFrame(
      NODE_ID, static_cast<uint8_t>(seq & 0xFF), payload, frame, sizeof(frame));

  if (len > 0) {
    loraBridge.sendBinaryFrame(frame, len);
    // Serial.print("[UART TX] binary seq=");
    // Serial.print(seq & 0xFF);
    // Serial.print(" node=");
    // Serial.println(NODE_ID);
  } else {
    Serial.println("[UART TX] encode failed");
  }
}
void serviceBridge() {
  if (loraBridge.hasError()) {
    Serial.print("[UART ERR] ");
    Serial.println(loraBridge.lastError());
  }

  if (loraBridge.bootSeen()) {
    static bool printedBoot = false;
    if (!printedBoot) {
      Serial.println("[UART] Feather boot seen");
      printedBoot = true;
    }
  }

  static uint32_t lastPrintedAck = 0;
  if (loraBridge.hasAck()) {
    const uint32_t ackSeq = loraBridge.lastAckSeq();

    if (ackSeq != lastPrintedAck) {
      // Serial.print("[UART ACK] seq=");
      // Serial.println(ackSeq);
      lastPrintedAck = ackSeq;
    }

    lastUARTAckedSeq = ackSeq;

    // Wire seq is 8-bit rolling (0-255); compare low byte only
    if (waitingForUARTAck && (ackSeq & 0xFF) == (lastSentSeq & 0xFF)) {
      waitingForUARTAck = false;
    }
  }

  if (loraBridge.hasRx()) {
    static char lastRxPrinted[96] = {0};
    if (strcmp(lastRxPrinted, loraBridge.lastRx()) != 0) {
      Serial.print("[UART RX PAYLOAD] ");
      Serial.println(loraBridge.lastRx());
      strncpy(lastRxPrinted, loraBridge.lastRx(), sizeof(lastRxPrinted) - 1);
      lastRxPrinted[sizeof(lastRxPrinted) - 1] = '\0';
    }
  }
}
