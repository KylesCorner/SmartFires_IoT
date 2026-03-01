#include <Arduino.h>

#include "ISensor.h"
#include "FlameSensor.h"
#include "DhtSensor.h"
#include "PassiveBuzzer.h"
#include "Icm20948Imu.h"
#include "PinMapping.h"

// Sensors
Icm20948Imu imu(1);
PassiveBuzzer buzzer(PIN_BUZZER); // choose a PWM-capable pin (not required for tone())
FlameSensor flame(PIN_FLAME_AO, PIN_FLAME_DO);
DhtSensor   dht(PIN_DHT, DHT_TYPE);

// Polymorphic registry
ISensor* sensors[] = { &flame, &dht, &imu };
constexpr size_t kNumSensors = sizeof(sensors) / sizeof(sensors[0]);

IActuator* actuators[] = { &buzzer };
constexpr size_t kNumActuators = sizeof(actuators) / sizeof(actuators[0]);

// Basic scheduling
uint32_t lastPrintMs = 0;

void setup() {
  Serial.begin(9600);
  delay(200);

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

  // Quick startup chirp
  buzzer.beep(2000, 20);
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
        buzzer.beep(1000, 500); // alert!
      }
    }


    Serial.print("[DHT] ");
    if (!dht.hasReading()) {
      Serial.println("no reading yet");
    } else {
      Serial.print("T=");
      Serial.print(dht.tempF(), 1);
      Serial.print("F H=");
      Serial.print(dht.humidity(), 1);
      Serial.println("%");
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

  }

  //delay(50);
}