#pragma once
#include "drivers/IGpsDriver.h"

class FakeGpsDriver final : public IGpsDriver {
public:
  bool beginOk = true;
  bool pollOk = true;
  bool readOk = true;
  Data data{};

  bool beginCalled = false;
  unsigned pollCount = 0;
  unsigned readCount = 0;

  bool begin() override {
    beginCalled = true;
    return beginOk;
  }

  bool poll() override {
    pollCount++;
    return pollOk;
  }

  bool read(Data &out) override {
    readCount++;
    out = data;
    return readOk;
  }
};
