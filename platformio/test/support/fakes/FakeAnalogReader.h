// ---
// description: Test double for IAnalogReader — returns a settable raw ADC count so BatteryMonitor can be constructed and driven without hardware.
// role: test-support
// ---
#pragma once

#include "interfaces/IAnalogReader.h"

class FakeAnalogReader : public IAnalogReader {
public:
  // Mid-scale by default: a plausible battery reading, so a test that only needs
  // BatteryMonitor to exist does not have to set anything up.
  int value = 512;
  uint16_t readCount = 0;
  uint8_t lastPin = 0xFF;

  int read(uint8_t pin) override {
    lastPin = pin;
    readCount++;
    return value;
  }
};
