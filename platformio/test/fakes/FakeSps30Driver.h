#pragma once
#include "drivers/ISps30Driver.h"

class FakeSps30Driver final : public ISps30Driver {
public:
  bool beginOk = true;
  bool startOk = true;
  bool stopOk = true;
  bool readOk = true;
  Data data{};

  bool beginCalled = false;
  unsigned startCount = 0;
  unsigned stopCount = 0;
  unsigned readCount = 0;

  bool begin() override {
    beginCalled = true;
    return beginOk;
  }

  bool startMeasurement() override {
    startCount++;
    return startOk;
  }

  bool stopMeasurement() override {
    stopCount++;
    return stopOk;
  }

  bool read(Data &out) override {
    readCount++;
    out = data;
    return readOk;
  }
};
