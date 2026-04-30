#pragma once

#include "drivers/IIcm20948Driver.h"

#include <stdint.h>

class FakeIcm20948Driver final : public IIcm20948Driver {
public:
  bool beginOk = true;
  bool readOk = true;

  uint8_t lastAddress = 0;
  uint32_t beginCount = 0;
  uint32_t readCount = 0;

  Data data{};

  bool begin(uint8_t address) override {
    beginCount++;
    lastAddress = address;
    return beginOk;
  }

  bool read(Data &out) override {
    readCount++;

    if (!readOk) {
      return false;
    }

    out = data;
    return true;
  }

  void setReading(float ax, float ay, float az, float gx, float gy, float gz,
                  float mx, float my, float mz, bool valid = true) {
    data.accelX = ax;
    data.accelY = ay;
    data.accelZ = az;

    data.gyroX = gx;
    data.gyroY = gy;
    data.gyroZ = gz;

    data.magX = mx;
    data.magY = my;
    data.magZ = mz;

    data.valid = valid;
  }
};
