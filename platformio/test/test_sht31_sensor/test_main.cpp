#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <unity.h>

#include "Sht31Sensor.h"
#include "II2CDevice.h"

static Sht31Sensor sht31(Sht31Sensor::kAlternateAddress);

namespace {
constexpr uint32_t kSampleDelayMs = 100;
constexpr int kWarmupSamples = 5;
constexpr int kAverageSamples = 10;

// Keep these fairly loose for real hardware.
constexpr float kMinReasonableTempC = -20.0f;
constexpr float kMaxReasonableTempC = 80.0f;
constexpr float kMinHumidityPct = 0.0f;
constexpr float kMaxHumidityPct = 100.0f;

// Heater trend thresholds. You may tune these after a real run.
constexpr float kMinHeaterTempRiseC = 0.2f;
constexpr float kMinHeaterHumidityDropPct = 0.5f;

bool sampleUntilReading(Sht31Sensor& sensor, int maxAttempts = 20) {
  for (int i = 0; i < maxAttempts; ++i) {
    sensor.sample();
    delay(kSampleDelayMs);
    if (sensor.hasReading()) {
      return true;
    }
  }
  return false;
}

void warmupSensor(Sht31Sensor& sensor) {
  for (int i = 0; i < kWarmupSamples; ++i) {
    sensor.sample();
    delay(kSampleDelayMs);
  }
}

float averageTemperatureC(Sht31Sensor& sensor, int count) {
  float sum = 0.0f;
  int used = 0;

  for (int i = 0; i < count; ++i) {
    sensor.sample();
    delay(kSampleDelayMs);

    if (sensor.hasReading()) {
      const float t = sensor.temperatureC();
      if (isfinite(t)) {
        sum += t;
        ++used;
      }
    }
  }

  TEST_ASSERT_GREATER_THAN_MESSAGE(0, used, "No valid temperature samples collected");
  return sum / static_cast<float>(used);
}

float averageHumidityPct(Sht31Sensor& sensor, int count) {
  float sum = 0.0f;
  int used = 0;

  for (int i = 0; i < count; ++i) {
    sensor.sample();
    delay(kSampleDelayMs);

    if (sensor.hasReading()) {
      const float h = sensor.humidityPct();
      if (isfinite(h)) {
        sum += h;
        ++used;
      }
    }
  }

  TEST_ASSERT_GREATER_THAN_MESSAGE(0, used, "No valid humidity samples collected");
  return sum / static_cast<float>(used);
}
}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_sht31_begin_returns_true() {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(), "SHT31 begin() failed");
}

void test_sht31_ping_reports_connected() {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(), "SHT31 begin() failed");
  TEST_ASSERT_TRUE_MESSAGE(sht31.ping(), "SHT31 ping() failed");
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      static_cast<int>(I2CStatus::Ok),
      static_cast<int>(sht31.lastI2CStatus()),
      "Expected lastI2CStatus() == I2CStatus::Ok after ping");
}

void test_sht31_produces_valid_reading() {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(), "SHT31 begin() failed");
  warmupSensor(sht31);

  TEST_ASSERT_TRUE_MESSAGE(
      sampleUntilReading(sht31),
      "SHT31 did not produce a reading");

  const float t = sht31.temperatureC();
  const float h = sht31.humidityPct();

  TEST_ASSERT_TRUE_MESSAGE(isfinite(t), "Temperature is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(h), "Humidity is not finite");
}

void test_sht31_temperature_is_reasonable() {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(), "SHT31 begin() failed");
  warmupSensor(sht31);

  TEST_ASSERT_TRUE_MESSAGE(
      sampleUntilReading(sht31),
      "SHT31 did not produce a reading");

  const float t = sht31.temperatureC();

  TEST_ASSERT_TRUE_MESSAGE(isfinite(t), "Temperature is not finite");
  TEST_ASSERT_TRUE_MESSAGE(t >= kMinReasonableTempC, "Temperature below reasonable lower bound");
  TEST_ASSERT_TRUE_MESSAGE(t <= kMaxReasonableTempC, "Temperature above reasonable upper bound");
}

void test_sht31_humidity_is_in_valid_range() {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(), "SHT31 begin() failed");
  warmupSensor(sht31);

  TEST_ASSERT_TRUE_MESSAGE(
      sampleUntilReading(sht31),
      "SHT31 did not produce a reading");

  const float h = sht31.humidityPct();

  TEST_ASSERT_TRUE_MESSAGE(isfinite(h), "Humidity is not finite");
  TEST_ASSERT_TRUE_MESSAGE(h >= kMinHumidityPct, "Humidity below 0%");
  TEST_ASSERT_TRUE_MESSAGE(h <= kMaxHumidityPct, "Humidity above 100%");
}

void test_sht31_fahrenheit_matches_celsius() {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(), "SHT31 begin() failed");
  warmupSensor(sht31);

  TEST_ASSERT_TRUE_MESSAGE(
      sampleUntilReading(sht31),
      "SHT31 did not produce a reading");

  const float c = sht31.temperatureC();
  const float f = sht31.temperatureF();
  const float expectedF = c * 9.0f / 5.0f + 32.0f;

  TEST_ASSERT_TRUE_MESSAGE(isfinite(c), "Temperature C is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(f), "Temperature F is not finite");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
      0.25f, expectedF, f, "temperatureF() does not match temperatureC()");
}

void test_sht31_consecutive_readings_are_stable() {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(), "SHT31 begin() failed");
  warmupSensor(sht31);

  TEST_ASSERT_TRUE_MESSAGE(
      sampleUntilReading(sht31),
      "SHT31 did not produce first reading");

  const float t1 = sht31.temperatureC();
  const float h1 = sht31.humidityPct();

  delay(200);

  TEST_ASSERT_TRUE_MESSAGE(
      sampleUntilReading(sht31),
      "SHT31 did not produce second reading");

  const float t2 = sht31.temperatureC();
  const float h2 = sht31.humidityPct();

  TEST_ASSERT_TRUE_MESSAGE(isfinite(t1), "First temperature is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(t2), "Second temperature is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(h1), "First humidity is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(h2), "Second humidity is not finite");

  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
      5.0f, t1, t2, "Temperature changed too sharply between consecutive samples");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
      15.0f, h1, h2, "Humidity changed too sharply between consecutive samples");
}

void test_sht31_heater_flag_toggles() {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(), "SHT31 begin() failed");

  sht31.setHeater(false);
  TEST_ASSERT_FALSE_MESSAGE(sht31.heaterEnabled(), "Heater should be OFF");

  sht31.setHeater(true);
  TEST_ASSERT_TRUE_MESSAGE(sht31.heaterEnabled(), "Heater should be ON");

  sht31.setHeater(false);
  TEST_ASSERT_FALSE_MESSAGE(sht31.heaterEnabled(), "Heater should be OFF again");
}

void test_sht31_heater_changes_sensor_readings() {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(), "SHT31 begin() failed");

  // Reset to a known state.
  sht31.setHeater(false);
  warmupSensor(sht31);

  TEST_ASSERT_TRUE_MESSAGE(
      sampleUntilReading(sht31),
      "SHT31 did not produce baseline reading");

  const float baselineTemp = averageTemperatureC(sht31, kAverageSamples);
  const float baselineHumidity = averageHumidityPct(sht31, kAverageSamples);

  sht31.setHeater(true);
  TEST_ASSERT_TRUE_MESSAGE(sht31.heaterEnabled(), "Heater should report enabled");

  // Give the heater time to influence the sensor.
  delay(1500);

  const float heatedTemp = averageTemperatureC(sht31, kAverageSamples);
  const float heatedHumidity = averageHumidityPct(sht31, kAverageSamples);

  sht31.setHeater(false);
  TEST_ASSERT_FALSE_MESSAGE(sht31.heaterEnabled(), "Heater should report disabled");

  TEST_ASSERT_TRUE_MESSAGE(
      heatedTemp > (baselineTemp + kMinHeaterTempRiseC),
      "Heater did not raise temperature enough");

  TEST_ASSERT_TRUE_MESSAGE(
      heatedHumidity < (baselineHumidity - kMinHeaterHumidityDropPct),
      "Heater did not lower humidity enough");
}

void test_sht31_reset_preserves_operability() {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(), "SHT31 begin() failed");

  sht31.reset();
  delay(50);

  TEST_ASSERT_TRUE_MESSAGE(
      sampleUntilReading(sht31),
      "SHT31 did not recover after reset");

  TEST_ASSERT_TRUE_MESSAGE(isfinite(sht31.temperatureC()), "Temperature invalid after reset");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(sht31.humidityPct()), "Humidity invalid after reset");
}

void setup() {
  delay(2000);
  Serial.begin(115200);
  Wire.begin();
  delay(200);

  UNITY_BEGIN();
  RUN_TEST(test_sht31_begin_returns_true);
  RUN_TEST(test_sht31_ping_reports_connected);
  RUN_TEST(test_sht31_produces_valid_reading);
  RUN_TEST(test_sht31_temperature_is_reasonable);
  RUN_TEST(test_sht31_humidity_is_in_valid_range);
  RUN_TEST(test_sht31_fahrenheit_matches_celsius);
  RUN_TEST(test_sht31_consecutive_readings_are_stable);
  RUN_TEST(test_sht31_heater_flag_toggles);
  RUN_TEST(test_sht31_heater_changes_sensor_readings);
  RUN_TEST(test_sht31_reset_preserves_operability);
  UNITY_END();
}

void loop() {}
