#pragma once
#include <stdint.h>

class IGpsDriver {
public:
  struct Data {
    bool fix = false;
    uint8_t fixQuality = 0;
    uint8_t satellites = 0;

    float latitudeDeg = 0.0f;
    float longitudeDeg = 0.0f;
    float altitudeM = 0.0f;

    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
  };

  virtual ~IGpsDriver() = default;

  virtual bool begin(uint8_t address) = 0;
  virtual bool poll() = 0;
  virtual bool read(Data &out) = 0;
};
