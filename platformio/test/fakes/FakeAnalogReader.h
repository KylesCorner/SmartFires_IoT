#pragma once

#include "interfaces/IAnalogReader.h"

class FakeAnalogReader final : public IAnalogReader {
public:
  int values[64] = {};
  unsigned readCounts[64] = {};

  int read(uint8_t pin) override {
    if (pin >= 64) {
      return -1;
    }

    readCounts[pin]++;
    return values[pin];
  }

  void set(uint8_t pin, int value) {
    if (pin < 64) {
      values[pin] = value;
    }
  }

  unsigned count(uint8_t pin) const {
    if (pin >= 64) {
      return 0;
    }

    return readCounts[pin];
  }
};
