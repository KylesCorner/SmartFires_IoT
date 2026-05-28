#pragma once

#include <stdint.h>

class IIcm20948Driver {
public:
  struct Data {
    float accelX = 0.0f;
    float accelY = 0.0f;
    float accelZ = 0.0f;

    float gyroX = 0.0f;
    float gyroY = 0.0f;
    float gyroZ = 0.0f;

    float magX = 0.0f;
    float magY = 0.0f;
    float magZ = 0.0f;

    // DMP Geomag output — populated instead of raw mag/accel/gyro in DMP mode.
    float   headingDeg      = 0.0f;
    int16_t headingAccuracy = 0;     // 0 (unreliable) – 3 (high)
    bool    headingValid    = false;

    bool valid = false;
  };

  virtual ~IIcm20948Driver() = default;

  virtual bool begin(uint8_t address) = 0;
  virtual bool read(Data &out) = 0;
};
