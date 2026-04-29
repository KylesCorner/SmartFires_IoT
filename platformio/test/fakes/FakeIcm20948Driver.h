#pragma once
#include "drivers/IIcm20948Driver.h"

class FakeIcm20948Driver final : public IIcm20948Driver {
public:
  bool beginOk = true;
  bool readOk = true;
  Data data{};

  bool beginCalled = false;
  unsigned readCount = 0;

  bool begin() override {
    beginCalled = true;
    return beginOk;
  }

  bool read(Data &out) override {
    readCount++;
    out = data;
    return readOk;
  }
};
