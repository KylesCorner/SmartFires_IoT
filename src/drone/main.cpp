#include <Arduino.h>

#include "ISensor.h"
#include "FlameSensor.h"
//#include "PassiveBuzzer.h"
#include "Icm20948Imu.h"
#include "PinMapping.h"
//#include "MicroServo.h"
#include "Sht31Sensor.h"
#include "Pa1010dGpsSensor.h"
#include "OledDisplay.h"
#include "USBCDC.h"
#include "WindSensorRevC.h"
#include "LidarLiteV3.h"
#include "shared/UartLoRaBridge.h"
#include "shared/TelemetryPacket.h"
#include "shared/TelemetryCodec.h"




// Sensors
WindSensorRevC wind(PIN_WIND_RV, PIN_WIND_TMP);
Icm20948Imu imu(1);
FlameSensor flame(PIN_FLAME_AO, PIN_FLAME_DO);
Sht31Sensor sht31(Sht31Sensor::kAlternateAddress);
Pa1010dGpsSensor gps(Wire);
LidarLiteV3 lidar;

//Actuators
//PassiveBuzzer buzzer(PIN_BUZZER); // choose a PWM-capable pin (not required for tone())
//MicroServo servo(PIN_SERVO); // choose a PWM-capable pin
OledDisplay oled(OledDisplay::Controller::SH1106, 0x3C, "OLED Display");

// Polymorphic registry
ISensor* sensors[] = { &flame, &imu , &sht31, &gps, &wind, &lidar };
constexpr size_t kNumSensors = sizeof(sensors) / sizeof(sensors[0]);

IActuator* actuators[] = { &oled };
constexpr size_t kNumActuators = sizeof(actuators) / sizeof(actuators[0]);

// Basic scheduling
uint32_t lastPrintMs = 0;

// oled page state and button debouncing state
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

bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
uint32_t lastDebounceMs = 0;
constexpr uint32_t kDebounceMs = 15;
void updateButton();

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
  TF_WIND  = 1 << 1,
  TF_SHT31 = 1 << 2,
  TF_LIDAR = 1 << 3,
  TF_GPS   = 1 << 4,
  TF_IMU   = 1 << 5
};

void sendTelemetry(uint32_t seq);
TelemetryPacket buildTelemetryPacket(uint32_t seq);
void handleBridgeLine(const char* line);
void serviceBridge();

void setup() {
  Serial.begin(115200);
  //Serial1.begin(115200);
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

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  loraBridge.begin();


  // Quick startup chirp
  //buzzer.beep(2000, 20);
}
void loop() {
  // Pull in any UART bytes and process complete lines
  loraBridge.update();
  serviceBridge();

  // Sample everything frequently (each sensor can self-throttle internally)
  for (size_t i = 0; i < kNumSensors; i++) {
    sensors[i]->sample();
  }

  // Update actuators
  for (size_t i = 0; i < kNumActuators; i++) {
    actuators[i]->update();
  }

  updateButton();

  const uint32_t now = millis();

  // Send telemetry at a controlled cadence
  if (now - lastSendMs >= UartLoRaBridge::kTelemetryPeriodMs) {
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
  if (waitingForUARTAck && (now - lastSendTimeMs >= UartLoRaBridge::kAckTimeoutMs)) {
    Serial.print("[UART] ACK timeout for seq ");
    Serial.println(lastSentSeq);
    waitingForUARTAck = false;
  }

  if (oled.healthy()) {
    switch (currentPage) {
      case PAGE_ENV:
        oled.printLine(0, "Environment");
        if (wind.hasReading()) {
          oled.printfLine(1, "Wind Speed:%.3f", wind.windMps());
        } else {
          oled.printLine(1, "Wind: Warming up");
        }

        if (sht31.hasReading()) {
          oled.printfLine(2, "T:%.1fF %.1fC H:%1.f%%",
                          sht31.temperatureF(),
                          sht31.temperatureC(),
                          sht31.humidityPct());
        } else {
          oled.printLine(2, "T/H:no reading");
        }

        if (flame.hasReading()) {
          oled.printfLine(3, "Flame:%d", flame.analogRaw());
        } else {
          oled.printLine(3, "No flame reading");
        }
        break;

      case PAGE_GPS:
        oled.printLine(0, "GPS");

        if (!gps.hasReading()) {
          oled.printLine(1, "No data");
          oled.printLine(2, "");
          oled.printLine(3, "");
        } else if (!gps.hasFix()) {
          oled.printfLine(1, "No fix S:%u", gps.satellites());
          oled.printfLine(2, "Age:%lu ms", (unsigned long)gps.sentenceAgeMs());
          oled.printLine(3, "Go outside");
        } else {
          oled.printfLine(1, "S:%u Alt:%.0fm",
                          gps.satellites(),
                          gps.altitudeMeters());
          oled.printfLine(2, "Lat:%.4f", gps.latitudeDegrees());
          oled.printfLine(3, "Lon:%.4f", gps.longitudeDegrees());
        }
        break;

      case PAGE_IMU:
        oled.printLine(0, "IMU");

        if (imu.hasReading()) {
          oled.printfLine(1, "A:%.1f %.1f %.1f",
                          imu.ax_mg(), imu.ay_mg(), imu.az_mg());
          oled.printfLine(2, "G:%.1f %.1f %.1f",
                          imu.gx_dps(), imu.gy_dps(), imu.gz_dps());
          oled.printfLine(3, "M:%.1f %.1f %.1f",
                          imu.mx_uT(), imu.my_uT(), imu.mz_uT());
        } else {
          oled.printLine(1, "No IMU reading");
          oled.printLine(2, "");
          oled.printLine(3, "");
        }
        break;

      case PAGE_UART:
        oled.printLine(0, "UART Connection");
        oled.printfLine(1, "Sent:%lu Ack:%lu",
                        (unsigned long)lastSentSeq,
                        (unsigned long)lastUARTAckedSeq);
        oled.printfLine(2, "WaitAck:%s", waitingForUARTAck ? "YES" : "NO");
        oled.printfLine(3, "UART:%s",
                        loraBridge.hasError() ? loraBridge.lastError() : "OK");
        break;

      case PAGE_LIDAR:
        oled.printLine(0, "LIDAR");
        oled.printLine(2, "");
        oled.printLine(3, "");
        if (lidar.hasReading()) {
          oled.printfLine(1, "Dist: %d cm", lidar.distanceCm());
        } else {
          oled.printLine(1, "No readings");
        }
        break;
      case PAGE_LORA: {
        oled.printLine(0, "LoRa Link");

        const bool bootSeen = loraBridge.bootSeen();
        const bool linkAlive =loraBridge.bootSeen() && ((millis() - lastSendTimeMs) < 3000 || waitingForUARTAck == false);

        oled.printfLine(1, "B:%s L:%s",
                        bootSeen ? "Y" : "N",
                        linkAlive ? "OK" : "WAIT");

        oled.printfLine(2, "S:%lu A:%lu",
                        (unsigned long)lastSentSeq,
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
}

void updateButton() {
  const bool reading = digitalRead(PIN_BUTTON);

  if (reading != lastButtonReading) {
    lastDebounceMs = millis();
  }

  if ((millis() - lastDebounceMs) > kDebounceMs) {
    if (reading != stableButtonState) {
      stableButtonState = reading;

      // Detect button press edge: HIGH -> LOW
      if (stableButtonState == LOW) {
        currentPage = (currentPage + 1) % PAGE_COUNT;
      }
    }
  }

  lastButtonReading = reading;
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

  char line[192];
  if (TelemetryCodec::encode(pkt, line, sizeof(line))) {
    loraBridge.sendLine(line);
    Serial.print("[UART TX] ");
    Serial.println(line);
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
      Serial.print("[UART ACK] seq=");
      Serial.println(ackSeq);
      lastPrintedAck = ackSeq;
    }

    lastUARTAckedSeq = ackSeq;

    if (waitingForUARTAck && ackSeq == lastSentSeq) {
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
