#include <Arduino.h>
#include <math.h>
#include <unity.h>

#include "WindSensorRevC.h"
#include "PinMapping.h"

namespace {
WindSensorRevC::Config makeTestConfig() {
  WindSensorRevC::Config cfg;
  cfg.pinRv = PIN_WIND_RV;
  cfg.pinTmp = PIN_WIND_TMP;
  cfg.adcRefVolts = 3.3f;
  cfg.adcMax = 4095;
  cfg.rvDividerRatio = 1.0f;
  cfg.tmpDividerRatio = 1.0f;
  cfg.zeroWindAdjustmentVolts = 0.2f;

  // Shorter timings for tests.
  cfg.warmupMs = 500;
  cfg.minSamplePeriodMs = 50;
  cfg.samplesToAverage = 4;
  return cfg;
}

WindSensorRevC wind(makeTestConfig());

constexpr uint32_t kPostWarmupDelayMs = 600;
constexpr uint32_t kSampleDelayMs = 60;
constexpr int kAverageSamples = 12;

// Loose sanity limits for real hardware.
constexpr float kMinWindMps = 0.0f;
constexpr float kMaxReasonableWindMps = 100.0f;
constexpr float kMinReasonableTempC = -40.0f;
constexpr float kMaxReasonableTempC = 100.0f;

// Idle stability threshold.
constexpr float kMaxIdleDeltaMps = 2.5f;

// Manual airflow threshold.
constexpr float kMinStimulusIncreaseMps = 0.5f;

bool collectReading(WindSensorRevC& sensor, int maxAttempts = 20) {
  for (int i = 0; i < maxAttempts; ++i) {
    if (sensor.sample() && sensor.hasReading()) {
      return true;
    }
    delay(kSampleDelayMs);
  }
  return false;
}

float averageWindMps(WindSensorRevC& sensor, int count) {
  float sum = 0.0f;
  int used = 0;

  for (int i = 0; i < count; ++i) {
    if (sensor.sample() && sensor.hasReading()) {
      const float v = sensor.windMps();
      if (isfinite(v)) {
        sum += v;
        ++used;
      }
    }
    delay(kSampleDelayMs);
  }

  TEST_ASSERT_GREATER_THAN_MESSAGE(0, used, "No valid wind samples collected");
  return sum / static_cast<float>(used);
}
}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_wind_begin_returns_true() {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "WindSensorRevC begin() failed");
}

void test_wind_not_ready_during_warmup() {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "WindSensorRevC begin() failed");

  TEST_ASSERT_FALSE_MESSAGE(
      wind.ready(),
      "Wind sensor should not be ready immediately after begin()");
}

void test_wind_sample_during_warmup_returns_false_and_no_reading() {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "WindSensorRevC begin() failed");

  const bool sampled = wind.sample();

  TEST_ASSERT_FALSE_MESSAGE(
      sampled,
      "sample() should return false during warmup");
  TEST_ASSERT_FALSE_MESSAGE(
      wind.hasReading(),
      "Sensor should not have a reading during warmup");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      UINT32_MAX,
      wind.ageMs(),
      "ageMs() should be UINT32_MAX before the first reading");
}

void test_wind_becomes_ready_after_warmup() {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "WindSensorRevC begin() failed");

  delay(kPostWarmupDelayMs);

  TEST_ASSERT_TRUE_MESSAGE(
      wind.ready(),
      "Wind sensor should be ready after warmup expires");
}

void test_wind_produces_valid_reading_after_warmup() {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "WindSensorRevC begin() failed");
  delay(kPostWarmupDelayMs);

  TEST_ASSERT_TRUE_MESSAGE(
      collectReading(wind),
      "Wind sensor did not produce a valid reading after warmup");

  TEST_ASSERT_TRUE_MESSAGE(wind.hasReading(), "Sensor should report hasReading()");
  TEST_ASSERT_TRUE_MESSAGE(wind.healthy(), "Sensor should be healthy after good reading");

  const float windMps = wind.windMps();
  const float tempC = wind.temperatureC();
  const float rvVolts = wind.rvVolts();
  const float tmpVolts = wind.tmpVolts();
  const float zeroWindVolts = wind.zeroWindVolts();

  TEST_ASSERT_TRUE_MESSAGE(isfinite(windMps), "windMps() is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(tempC), "temperatureC() is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(rvVolts), "rvVolts() is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(tmpVolts), "tmpVolts() is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(zeroWindVolts), "zeroWindVolts() is not finite");

  TEST_ASSERT_TRUE_MESSAGE(windMps >= kMinWindMps, "windMps() should not be negative");
  TEST_ASSERT_TRUE_MESSAGE(windMps <= kMaxReasonableWindMps, "windMps() exceeds reasonable upper bound");

  TEST_ASSERT_TRUE_MESSAGE(tempC >= kMinReasonableTempC, "temperatureC() below reasonable lower bound");
  TEST_ASSERT_TRUE_MESSAGE(tempC <= kMaxReasonableTempC, "temperatureC() above reasonable upper bound");
}

void test_wind_temperature_f_matches_c() {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "WindSensorRevC begin() failed");
  delay(kPostWarmupDelayMs);

  TEST_ASSERT_TRUE_MESSAGE(
      collectReading(wind),
      "Wind sensor did not produce a valid reading");

  const float c = wind.temperatureC();
  const float f = wind.temperatureF();
  const float expectedF = c * 9.0f / 5.0f + 32.0f;

  TEST_ASSERT_TRUE_MESSAGE(isfinite(c), "temperatureC() is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(f), "temperatureF() is not finite");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
      0.25f, expectedF, f, "temperatureF() does not match temperatureC()");
}

void test_wind_age_ms_updates_after_good_sample() {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "WindSensorRevC begin() failed");
  delay(kPostWarmupDelayMs);

  TEST_ASSERT_TRUE_MESSAGE(
      collectReading(wind),
      "Wind sensor did not produce a valid reading");

  const uint32_t age0 = wind.ageMs();
  delay(100);
  const uint32_t age1 = wind.ageMs();

  TEST_ASSERT_TRUE_MESSAGE(age0 < UINT32_MAX, "ageMs() should be valid after reading");
  TEST_ASSERT_TRUE_MESSAGE(age1 >= age0, "ageMs() should increase over time");
}

void test_wind_idle_readings_are_stable() {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "WindSensorRevC begin() failed");
  delay(kPostWarmupDelayMs);

  TEST_ASSERT_TRUE_MESSAGE(
      collectReading(wind),
      "Wind sensor did not produce first reading");

  const float v1 = wind.windMps();

  delay(300);

  TEST_ASSERT_TRUE_MESSAGE(
      collectReading(wind),
      "Wind sensor did not produce second reading");

  const float v2 = wind.windMps();

  TEST_ASSERT_TRUE_MESSAGE(isfinite(v1), "First wind reading is not finite");
  TEST_ASSERT_TRUE_MESSAGE(isfinite(v2), "Second wind reading is not finite");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
      kMaxIdleDeltaMps, v1, v2,
      "Idle wind readings changed too sharply");
}

void test_wind_zero_adjustment_keeps_output_non_negative() {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "WindSensorRevC begin() failed");
  delay(kPostWarmupDelayMs);

  wind.setZeroWindAdjustmentVolts(0.5f);

  TEST_ASSERT_TRUE_MESSAGE(
      collectReading(wind),
      "Wind sensor did not produce a reading after zero-wind adjustment");

  TEST_ASSERT_TRUE_MESSAGE(
      wind.windMps() >= 0.0f,
      "windMps() should remain non-negative after zero-wind adjustment");
}

// Manual airflow test.
void test_wind_increases_with_airflow() {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "WindSensorRevC begin() failed");
  delay(kPostWarmupDelayMs);

  const float baseline = averageWindMps(wind, kAverageSamples);

  Serial.println();
  Serial.println(">>> WIND TEST: Blow steadily across the front of the wind sensor now...");
  delay(3000);

  const float stimulated = averageWindMps(wind, kAverageSamples);

  Serial.print("Baseline wind m/s: ");
  Serial.println(baseline, 4);
  Serial.print("Stimulated wind m/s: ");
  Serial.println(stimulated, 4);

  TEST_ASSERT_TRUE_MESSAGE(
      stimulated > (baseline + kMinStimulusIncreaseMps),
      "Wind reading did not increase enough under airflow stimulus");
}

void setup() {
  delay(2000);
  Serial.begin(115200);
  delay(200);

  UNITY_BEGIN();
  RUN_TEST(test_wind_begin_returns_true);
  RUN_TEST(test_wind_not_ready_during_warmup);
  RUN_TEST(test_wind_sample_during_warmup_returns_false_and_no_reading);
  RUN_TEST(test_wind_becomes_ready_after_warmup);
  RUN_TEST(test_wind_produces_valid_reading_after_warmup);
  RUN_TEST(test_wind_temperature_f_matches_c);
  RUN_TEST(test_wind_age_ms_updates_after_good_sample);
  RUN_TEST(test_wind_idle_readings_are_stable);
  RUN_TEST(test_wind_zero_adjustment_keeps_output_non_negative);

  // Manual interactive test.
  RUN_TEST(test_wind_increases_with_airflow);

  UNITY_END();
}

void loop() {}
