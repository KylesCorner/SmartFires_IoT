#include <Arduino.h>
#include <Wire.h>
#include <unity.h>

#include "Sht31Sensor.h"
#include "Pa1010dGpsSensor.h"
#include "Icm20948Imu.h"
#include "WindSensorRevC.h"
#include "PinMapping.h"

static Sht31Sensor sht31(Sht31Sensor::kAlternateAddress);
static Pa1010dGpsSensor gps(Wire);
static Icm20948Imu imu(1);
static WindSensorRevC wind(PIN_WIND_RV, PIN_WIND_TMP);

// Warmup / settle times
static constexpr uint32_t SHT31_WARMUP_MS = 50;
static constexpr uint32_t GPS_WARMUP_MS   = 500;
static constexpr uint32_t IMU_WARMUP_MS   = 100;
static constexpr uint32_t WIND_WARMUP_MS  = 10000;

// Retry windows for sample() after warmup
static constexpr uint32_t SHT31_SAMPLE_TIMEOUT_MS = 500;
static constexpr uint32_t GPS_SAMPLE_TIMEOUT_MS   = 3000;
static constexpr uint32_t IMU_SAMPLE_TIMEOUT_MS   = 500;
static constexpr uint32_t WIND_SAMPLE_TIMEOUT_MS  = 1000;

static constexpr uint32_t SAMPLE_RETRY_STEP_MS = 50;

void setUp(void) {}
void tearDown(void) {}

static bool waitForSample(ISensor& sensor, uint32_t warmupMs, uint32_t timeoutMs) {
  delay(warmupMs);

  const uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    if (sensor.sample()) {
      return true;
    }
    delay(SAMPLE_RETRY_STEP_MS);
  }

  return false;
}

void test_sht31_sleep_wake(void) {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(Wire), "SHT31 begin() failed");

  TEST_ASSERT_TRUE_MESSAGE(sht31.wake(), "SHT31 wake() failed before test");
  TEST_ASSERT_TRUE_MESSAGE(
    waitForSample(sht31, SHT31_WARMUP_MS, SHT31_SAMPLE_TIMEOUT_MS),
    "SHT31 sample() failed before sleep"
  );

  TEST_ASSERT_TRUE_MESSAGE(sht31.sleep(), "SHT31 sleep() failed");
  TEST_ASSERT_FALSE_MESSAGE(sht31.sample(), "SHT31 sample() should fail while sleeping");

  TEST_ASSERT_TRUE_MESSAGE(sht31.wake(), "SHT31 wake() failed");
  TEST_ASSERT_TRUE_MESSAGE(
    waitForSample(sht31, SHT31_WARMUP_MS, SHT31_SAMPLE_TIMEOUT_MS),
    "SHT31 sample() failed after wake"
  );
}

void test_gps_sleep_wake(void) {
  TEST_ASSERT_TRUE_MESSAGE(gps.begin(Wire), "GPS begin() failed");

  TEST_ASSERT_TRUE_MESSAGE(gps.wake(), "GPS wake() failed before test");
  TEST_ASSERT_TRUE_MESSAGE(
    waitForSample(gps, GPS_WARMUP_MS, GPS_SAMPLE_TIMEOUT_MS),
    "GPS sample() failed before sleep"
  );

  TEST_ASSERT_TRUE_MESSAGE(gps.sleep(), "GPS sleep() failed");
  TEST_ASSERT_FALSE_MESSAGE(gps.sample(), "GPS sample() should fail while sleeping");

  TEST_ASSERT_TRUE_MESSAGE(gps.wake(), "GPS wake() failed");
  TEST_ASSERT_TRUE_MESSAGE(
    waitForSample(gps, GPS_WARMUP_MS, GPS_SAMPLE_TIMEOUT_MS),
    "GPS did not resume sampling after wake"
  );
}

void test_imu_sleep_wake(void) {
  TEST_ASSERT_TRUE_MESSAGE(imu.begin(Wire), "IMU begin() failed");

  TEST_ASSERT_TRUE_MESSAGE(imu.wake(), "IMU wake() failed before test");
  TEST_ASSERT_TRUE_MESSAGE(
    waitForSample(imu, IMU_WARMUP_MS, IMU_SAMPLE_TIMEOUT_MS),
    "IMU sample() failed before sleep"
  );

  TEST_ASSERT_TRUE_MESSAGE(imu.sleep(), "IMU sleep() failed");
  TEST_ASSERT_FALSE_MESSAGE(imu.sample(), "IMU sample() should fail while sleeping");

  TEST_ASSERT_TRUE_MESSAGE(imu.wake(), "IMU wake() failed");
  TEST_ASSERT_TRUE_MESSAGE(
    waitForSample(imu, IMU_WARMUP_MS, IMU_SAMPLE_TIMEOUT_MS),
    "IMU sample() failed after wake"
  );
}

void test_wind_sleep_wake(void) {
  TEST_ASSERT_TRUE_MESSAGE(wind.begin(), "Wind begin() failed");

  TEST_ASSERT_TRUE_MESSAGE(wind.wake(), "Wind wake() failed before test");
  TEST_ASSERT_TRUE_MESSAGE(
    waitForSample(wind, WIND_WARMUP_MS, WIND_SAMPLE_TIMEOUT_MS),
    "Wind sample() failed before sleep"
  );

  TEST_ASSERT_TRUE_MESSAGE(wind.sleep(), "Wind sleep() failed");
  TEST_ASSERT_FALSE_MESSAGE(wind.sample(), "Wind sample() should fail while sleeping");

  TEST_ASSERT_TRUE_MESSAGE(wind.wake(), "Wind wake() failed");
  TEST_ASSERT_TRUE_MESSAGE(
    waitForSample(wind, WIND_WARMUP_MS, WIND_SAMPLE_TIMEOUT_MS),
    "Wind sample() failed after wake"
  );
}

void setup() {
  delay(2000);
  Serial.begin(115200);
  Wire.begin();
  delay(200);

  UNITY_BEGIN();
  RUN_TEST(test_sht31_sleep_wake);
  RUN_TEST(test_gps_sleep_wake);
  RUN_TEST(test_imu_sleep_wake);
  RUN_TEST(test_wind_sleep_wake);
  UNITY_END();
}

void loop() {}
