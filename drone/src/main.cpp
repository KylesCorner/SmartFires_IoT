#include <Arduino.h>

#include "ISensor.h"
#include "FlameSensor.h"
#include "PassiveBuzzer.h"
#include "Icm20948Imu.h"
#include "PinMapping.h"
#include "MicroServo.h"
#include "Sht31Sensor.h"
#include "Pa1010dGpsSensor.h"
#include "OledDisplay.h"
#include "WindSensorRevC.h"

// Optional UART telemetry stream for the Feather LoRa bridge.
// Set DEMO_TEST_SENSOR_DATA=1 via build_flags to enable.
#ifndef DEMO_TEST_SENSOR_DATA
#define DEMO_TEST_SENSOR_DATA 0
#endif

#if DEMO_TEST_SENSOR_DATA
static constexpr uint32_t kTelemetryIntervalMs = 1000;
static constexpr uint8_t kInterboardI2CAddr = 0x42;
static constexpr uint8_t kInterboardI2CMaxPayload = 63;
uint32_t lastTelemetryMs = 0;
uint32_t telemetrySeq = 0;

const char* i2cStatusToText(uint8_t status) {
  switch (status) {
    case 0: return "ok";
    case 1: return "data-too-long";
    case 2: return "nack-on-address";
    case 3: return "nack-on-data";
    case 4: return "other-error";
    default: return "unknown";
  }
}

void sendDemoTestSensorData(uint32_t now);
#endif

// Sensors
WindSensorRevC wind(A1, A0);
Icm20948Imu imu(1);
FlameSensor flame(PIN_FLAME_AO, PIN_FLAME_DO);
Sht31Sensor sht31(Sht31Sensor::kAlternateAddress);
Pa1010dGpsSensor gps(Wire);

//Actuators
//PassiveBuzzer buzzer(PIN_BUZZER); // choose a PWM-capable pin (not required for tone())
//MicroServo servo(PIN_SERVO); // choose a PWM-capable pin
OledDisplay oled(OledDisplay::Controller::SH1106, 0x3C, "OLED Display");

// Polymorphic registry
ISensor* sensors[] = { &flame, &imu , &sht31, &gps, &wind };
constexpr size_t kNumSensors = sizeof(sensors) / sizeof(sensors[0]);

IActuator* actuators[] = { &oled };
constexpr size_t kNumActuators = sizeof(actuators) / sizeof(actuators[0]);

#if DEMO_TEST_SENSOR_DATA
void sendDemoTestSensorData(uint32_t now) {
  char line[kInterboardI2CMaxPayload + 1];

  const bool gpsFix = gps.hasFix();
  const bool flameDetected = flame.hasReading() ? flame.detected() : false;
  const int flameAo = flame.hasReading() ? flame.analogRaw() : -1;

  const float tempC = sht31.hasReading() ? sht31.temperatureC() : NAN;
  const float humPct = sht31.hasReading() ? sht31.humidityPct() : NAN;

      // Compact frame with improved numeric precision that fits I2C payload limits.
  const unsigned long seqMod = static_cast<unsigned long>(telemetrySeq++ % 100000UL);
  const int len = snprintf(
      line,
      sizeof(line),
        "s=%lu,t=%.2f,h=%.2f,f=%u,a=%d,g=%u,n=%u",
      seqMod,
      tempC,
      humPct,
      static_cast<unsigned>(flameDetected ? 1 : 0),
      flameAo,
      static_cast<unsigned>(gpsFix ? 1 : 0),
      static_cast<unsigned>(gps.satellites()));

  if (len <= 0) {
    Serial.println("TO_FEATHER: format-failed");
    return;
  }

  if (len >= static_cast<int>(sizeof(line))) {
    Serial.println("TO_FEATHER: payload-truncated, dropped");
    return;
  }

  Wire.beginTransmission(kInterboardI2CAddr);
  Wire.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(len));
  const uint8_t txStatus = Wire.endTransmission();

  Serial.print("TO_FEATHER: ");
  Serial.print(line);
  Serial.print(" | i2c_status=");
  Serial.print(txStatus);
  Serial.print(" (");
  Serial.print(i2cStatusToText(txStatus));
  Serial.println(")");
}
#endif

// Basic scheduling
uint32_t lastPrintMs = 0;

// oled page state and button debouncing state
enum OledPage : uint8_t {
  PAGE_ENV = 0,
  PAGE_GPS,
  PAGE_IMU,
  PAGE_COUNT
};

uint8_t currentPage = PAGE_ENV;

bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
uint32_t lastDebounceMs = 0;
constexpr uint32_t kDebounceMs = 40;
void updateButton();


void setup() {
  Serial.begin(9600);
  delay(200);
  Wire.begin();

#if DEMO_TEST_SENSOR_DATA
  Serial.println("DEMO_TEST_SENSOR_DATA enabled: streaming I2C telemetry to addr 0x42");
#endif

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

  // Quick startup chirp
  //buzzer.beep(2000, 20);
}

void loop() {
  // Sample everything frequently (each sensor can self-throttle internally)
  for (size_t i = 0; i < kNumSensors; i++) {
    sensors[i]->sample();
  }

  // Update actuators (e.g. buzzer timing)
  for (size_t i = 0; i < kNumActuators; i++) {
    actuators[i]->update();
  }

  updateButton();

  // Print at a slower cadence (don’t spam serial)
  const uint32_t now = millis();
  if (now - lastPrintMs >= 1000) {
    lastPrintMs = now;

    Serial.println("\n=== Sensor Readings ===");

    if(!flame.hasReading()) {
      Serial.print("[Flame] no reading yet");
    } else {

      Serial.print("[Flame] detected=");
      Serial.print(flame.detected() ? "YES" : "NO");
      Serial.print(" AO=");
      Serial.print(flame.analogRaw());
      Serial.print(" DO=");
      Serial.println(flame.digitalRaw() ? "HIGH" : "LOW");

      if(flame.detected()) {
        //buzzer.beep(1000, 500); // alert!
      }
    }
    
    Serial.print("[Wind] ");
    if(!wind.hasReading()){
      Serial.println("No reading");
    }else{
      Serial.print("Wind: ");
      Serial.print(wind.windMps(), 3);
      Serial.print(" m/s  Temp: ");
      Serial.println(wind.temperatureC(), 2);
    }



    Serial.print("[GPS] ");
    if(!gps.hasReading()) {
      Serial.println("no reading yet");
    } else if (!gps.hasFix()) {
      Serial.println("no fix yet");
    } else {
      Serial.print("Lat: ");
      Serial.print(gps.latitudeDegrees(), 6);
      Serial.print(" Lon: ");
      Serial.print(gps.longitudeDegrees(), 6);
      Serial.print(" Alt(m): ");
      Serial.print(gps.altitudeMeters(), 1);
      Serial.print(" Sats: ");
      Serial.print(gps.satellites());
      Serial.print(" Age(ms): ");
      Serial.println(gps.ageMs());
    }

    Serial.print("[SHT31] ");
    if (sht31.hasReading()) {
    Serial.print("Temp F: ");
    Serial.print(sht31.temperatureF(), 2);
    Serial.print(" | Humidity %: ");
    Serial.print(sht31.humidityPct(), 2);
    Serial.print(" | Age ms: ");
    Serial.println(sht31.ageMs());
  } else {
    Serial.print("Sample failed. healthy=");
    Serial.print(sht31.healthy() ? "true" : "false");
    Serial.print(" ping=");
    Serial.print(sht31.ping() ? "true" : "false");
    Serial.print(" i2cStatus=");
    Serial.println(static_cast<uint8_t>(sht31.lastI2CStatus()));
  }


    Serial.print("[IMU] ");
    if(!imu.hasReading()) {
      Serial.println("no reading yet");
    } else {
      Serial.print("A=(mg) ");
      Serial.print(imu.ax_mg(), 1);
      Serial.print(",");
      Serial.print(imu.ay_mg(), 1);
      Serial.print(",");
      Serial.print(imu.az_mg(), 1);

      Serial.print(" G=(dps) ");
      Serial.print(imu.gx_dps(), 1);
      Serial.print(",");
      Serial.print(imu.gy_dps(), 1);
      Serial.print(",");
      Serial.print(imu.gz_dps(), 1);

      Serial.print(" M=(uT) ");
      Serial.print(imu.mx_uT(), 1);
      Serial.print(",");
      Serial.print(imu.my_uT(), 1);
      Serial.print(",");
      Serial.print(imu.mz_uT(), 1);

      Serial.print(" T=");
      Serial.print(imu.temp_C(), 1);
      Serial.println("C");
    } 

    Serial.println("====================\n");

  if (oled.healthy()) {
    switch (currentPage) {
      case PAGE_ENV:
        oled.printLine(0, "Environment");
        if (wind.hasReading()) {
          oled.printfLine(1, "Wind:%s AO:%d",
                          flame.detected() ? "YES" : "NO",
                          flame.analogRaw());
          oled.printfLine(1,"Wind Speed:%.3f", wind.windMps());
        } else {
          oled.printLine(1, "Wind: No Reading");
        }

        if (sht31.hasReading()) {
          oled.printfLine(2, "Temperature:%.1fF", sht31.temperatureF());
          oled.printfLine(3, "Humidity:%.1f%%", sht31.humidityPct());
        } else {
          oled.printLine(2, "T/H:no reading");
          oled.printLine(3, "");
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
          oled.printfLine(1, "A:%d %d %d",
                          (int)imu.ax_mg(),
                          (int)imu.ay_mg(),
                          (int)imu.az_mg());

          oled.printfLine(2, "G:%d %d %d",
                          (int)imu.gx_dps(),
                          (int)imu.gy_dps(),
                          (int)imu.gz_dps());
          oled.printfLine(3, "M %.0f %.1f %.1f",
                          (int)imu.mx_uT(),
                          (int)imu.my_uT(),
                          (int)imu.mz_uT());
        } else {
          oled.printLine(1, "No IMU reading");
          oled.printLine(2, "");
          oled.printLine(3, "");
        }
        break;
    }
  }

  }

#if DEMO_TEST_SENSOR_DATA
  if (now - lastTelemetryMs >= kTelemetryIntervalMs) {
    lastTelemetryMs = now;
    sendDemoTestSensorData(now);
  }
#endif

  //delay(50);
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