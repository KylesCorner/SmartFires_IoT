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

    bool valid = false;
  };

  virtual ~IIcm20948Driver() = default;

  virtual bool begin(uint8_t address) = 0;
  virtual bool read(Data &out) = 0;
};
