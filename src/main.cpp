#include <Arduino.h>

#include "ISensor.h"
#include "FlameSensor.h"
#include "DhtSensor.h"

// Pins (adjust to your wiring)
static constexpr uint8_t PIN_FLAME_AO = A0;
static constexpr uint8_t PIN_FLAME_DO = 3;

static constexpr uint8_t PIN_DHT = 2;
static constexpr uint8_t DHT_TYPE = DHT11; // or DHT22

FlameSensor flame(PIN_FLAME_AO, PIN_FLAME_DO);
DhtSensor   dht(PIN_DHT, DHT_TYPE);

// Polymorphic registry
ISensor* sensors[] = { &flame, &dht };
constexpr size_t kNumSensors = sizeof(sensors) / sizeof(sensors[0]);

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
}

void loop() {
  // Sample everything frequently (each sensor can self-throttle internally)
  for (size_t i = 0; i < kNumSensors; i++) {
    sensors[i]->sample();
  }

  // Print at a slower cadence (don’t spam serial)
  const uint32_t now = millis();
  if (now - lastPrintMs >= 1000) {
    lastPrintMs = now;

    Serial.print("[Flame] detected=");
    Serial.print(flame.detected() ? "YES" : "NO");
    Serial.print(" AO=");
    Serial.print(flame.analogRaw());
    Serial.print(" DO=");
    Serial.print(flame.digitalRaw() ? "HIGH" : "LOW");

    Serial.print(" | [DHT] ");
    if (!dht.hasReading()) {
      Serial.println("no reading yet");
    } else {
      Serial.print("T=");
      Serial.print(dht.tempC(), 1);
      Serial.print("C H=");
      Serial.print(dht.humidity(), 1);
      Serial.println("%");
    }
  }

  delay(50);
}