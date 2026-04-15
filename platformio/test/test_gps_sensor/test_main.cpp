#include <Arduino.h>
#include <Wire.h>
#include <unity.h>

#include "Pa1010dGpsSensor.h"

static Pa1010dGpsSensor gps(Wire);

// Adjust as needed for your antenna / sky view
static constexpr uint32_t GPS_BOOT_WARMUP_MS    = 1000;
static constexpr uint32_t GPS_SAMPLE_TIMEOUT_MS = 15000;
static constexpr uint32_t GPS_WAKE_WARMUP_MS    = 500;
static constexpr uint32_t GPS_RETRY_STEP_MS     = 100;

void setUp(void) {}
void tearDown(void) {}

static bool waitForParsedSentence(Pa1010dGpsSensor& sensor, uint32_t timeoutMs) {
  const uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    if (sensor.sample()) {
      return true;
    }
    delay(GPS_RETRY_STEP_MS);
  }
  return false;
}

static bool waitForValidUtcTime(Pa1010dGpsSensor& sensor, uint32_t timeoutMs) {
  const uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    sensor.sample();

    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint16_t millisecond = 0;

    if (sensor.getUtcTime(hour, minute, second, millisecond)) {
      return true;
    }

    delay(GPS_RETRY_STEP_MS);
  }
  return false;
}

static bool waitForPositionData(Pa1010dGpsSensor& sensor, uint32_t timeoutMs) {
  const uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    sensor.sample();

    const float lat = sensor.latitudeDegrees();
    const float lon = sensor.longitudeDegrees();
    const uint8_t sats = sensor.satellites();

    const bool latValid = !isnan(lat) && lat >= -90.0f && lat <= 90.0f;
    const bool lonValid = !isnan(lon) && lon >= -180.0f && lon <= 180.0f;
    const bool satsValid = sats > 0;

    if (latValid && lonValid && satsValid) {
      return true;
    }

    delay(GPS_RETRY_STEP_MS);
  }
  return false;
}

void test_gps_begin(void) {
  TEST_ASSERT_TRUE_MESSAGE(gps.begin(Wire), "GPS begin() failed");
}

void test_gps_time_position_and_satellites(void) {
  TEST_ASSERT_TRUE_MESSAGE(gps.begin(Wire), "GPS begin() failed");

  delay(GPS_BOOT_WARMUP_MS);

  TEST_ASSERT_TRUE_MESSAGE(
    waitForParsedSentence(gps, GPS_SAMPLE_TIMEOUT_MS),
    "GPS did not produce any parsed sentence"
  );

  TEST_ASSERT_TRUE_MESSAGE(
    waitForValidUtcTime(gps, GPS_SAMPLE_TIMEOUT_MS),
    "GPS did not produce valid UTC time"
  );

  TEST_ASSERT_TRUE_MESSAGE(
    waitForPositionData(gps, GPS_SAMPLE_TIMEOUT_MS),
    "GPS did not produce valid lat/lon/satellite data"
  );

  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint16_t millisecond = 0;

  TEST_ASSERT_TRUE_MESSAGE(
    gps.getUtcTime(hour, minute, second, millisecond),
    "getUtcTime() failed"
  );

  TEST_ASSERT_LESS_OR_EQUAL_UINT8_MESSAGE(23, hour, "GPS hour out of range");
  TEST_ASSERT_LESS_OR_EQUAL_UINT8_MESSAGE(59, minute, "GPS minute out of range");
  TEST_ASSERT_LESS_OR_EQUAL_UINT8_MESSAGE(59, second, "GPS second out of range");
  TEST_ASSERT_LESS_OR_EQUAL_UINT16_MESSAGE(999, millisecond, "GPS millisecond out of range");

  const float lat = gps.latitudeDegrees();
  const float lon = gps.longitudeDegrees();
  const uint8_t sats = gps.satellites();

  TEST_ASSERT_FALSE_MESSAGE(isnan(lat), "Latitude is NaN");
  TEST_ASSERT_FALSE_MESSAGE(isnan(lon), "Longitude is NaN");
  TEST_ASSERT_TRUE_MESSAGE(lat >= -90.0f && lat <= 90.0f, "Latitude out of range");
  TEST_ASSERT_TRUE_MESSAGE(lon >= -180.0f && lon <= 180.0f, "Longitude out of range");
  TEST_ASSERT_TRUE_MESSAGE(sats > 0, "Satellite count must be > 0");

  Serial.printf("GPS UTC time: %02u:%02u:%02u.%03u\n",
                hour, minute, second, millisecond);
  Serial.printf("GPS position: lat=%.6f lon=%.6f sats=%u\n",
                lat, lon, sats);
}

void test_gps_sleep_wake_and_resume_data(void) {
  TEST_ASSERT_TRUE_MESSAGE(gps.begin(Wire), "GPS begin() failed");

  delay(GPS_BOOT_WARMUP_MS);

  TEST_ASSERT_TRUE_MESSAGE(
    waitForParsedSentence(gps, GPS_SAMPLE_TIMEOUT_MS),
    "GPS did not produce parsed data before sleep"
  );

  TEST_ASSERT_TRUE_MESSAGE(gps.sleep(), "GPS sleep() failed");
  TEST_ASSERT_FALSE_MESSAGE(gps.sample(), "GPS sample() should fail while sleeping");

  TEST_ASSERT_TRUE_MESSAGE(gps.wake(), "GPS wake() failed");
  delay(GPS_WAKE_WARMUP_MS);

  TEST_ASSERT_TRUE_MESSAGE(
    waitForParsedSentence(gps, GPS_SAMPLE_TIMEOUT_MS),
    "GPS did not resume parsed data after wake"
  );

  TEST_ASSERT_TRUE_MESSAGE(
    waitForValidUtcTime(gps, GPS_SAMPLE_TIMEOUT_MS),
    "GPS did not resume valid UTC time after wake"
  );

  TEST_ASSERT_TRUE_MESSAGE(
    waitForPositionData(gps, GPS_SAMPLE_TIMEOUT_MS),
    "GPS did not resume valid lat/lon/satellite data after wake"
  );

  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint16_t millisecond = 0;

  TEST_ASSERT_TRUE_MESSAGE(
    gps.getUtcTime(hour, minute, second, millisecond),
    "getUtcTime() failed after wake"
  );

  const float lat = gps.latitudeDegrees();
  const float lon = gps.longitudeDegrees();
  const uint8_t sats = gps.satellites();

  TEST_ASSERT_TRUE_MESSAGE(lat >= -90.0f && lat <= 90.0f, "Latitude out of range after wake");
  TEST_ASSERT_TRUE_MESSAGE(lon >= -180.0f && lon <= 180.0f, "Longitude out of range after wake");
  TEST_ASSERT_TRUE_MESSAGE(sats > 0, "Satellite count must be > 0 after wake");

  Serial.printf("GPS UTC time after wake: %02u:%02u:%02u.%03u\n",
                hour, minute, second, millisecond);
  Serial.printf("GPS position after wake: lat=%.6f lon=%.6f sats=%u\n",
                lat, lon, sats);
}

void setup() {
  delay(2000);
  Serial.begin(115200);
  Wire.begin();
  delay(200);

  UNITY_BEGIN();
  RUN_TEST(test_gps_begin);
  RUN_TEST(test_gps_time_position_and_satellites);
  RUN_TEST(test_gps_sleep_wake_and_resume_data);
  UNITY_END();
}

void loop() {}
