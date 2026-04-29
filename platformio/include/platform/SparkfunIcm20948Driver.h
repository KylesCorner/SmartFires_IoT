#pragma once

#include "drivers/IIcm20948Driver.h"

#include <ICM_20948.h>
#include <Wire.h>

class SparkfunIcm20948Driver final : public IIcm20948Driver {
public:
  explicit SparkfunIcm20948Driver(TwoWire &wire = Wire);

  bool begin(uint8_t ad0Val) override;
  bool read(Data &out) override;

private:
  bool configureNormalMode();

  ICM_20948_I2C _imu;
  TwoWire &_wire;
  bool _begun = false;
};
