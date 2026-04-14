#include <Arduino.h>
#include <Wire.h>
#include <unity.h>

#include "Sht31Sensor.h"
#include "LidarLiteV3.h"
#include "Pa1010dGpsSensor.h"
#include "Icm20948Imu.h"

static Sht31Sensor sht31(Sht31Sensor::kAlternateAddress);
static LidarLiteV3 lidar;
static Pa1010dGpsSensor gps(Wire);
static Icm20948Imu imu(1);

void setUp(void) {}
void tearDown(void) {}

void test_all_i2c_devices_begin(void) {
  TEST_ASSERT_TRUE_MESSAGE(sht31.begin(Wire), "SHT31 begin() failed");
  // TEST_ASSERT_TRUE_MESSAGE(lidar.begin(Wire), "LIDAR begin() failed");
  TEST_ASSERT_TRUE_MESSAGE(gps.begin(Wire), "GPS begin() failed");
  TEST_ASSERT_TRUE_MESSAGE(imu.begin(Wire), "IMU begin() failed");
}

void setup() {
  delay(2000);
  Serial.begin(115200);
  Wire.begin();
  delay(200);

  UNITY_BEGIN();
  RUN_TEST(test_all_i2c_devices_begin);
  UNITY_END();
}

void loop() {}
