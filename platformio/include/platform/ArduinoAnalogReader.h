#pragma once

#include "interfaces/IAnalogReader.h"

class ArduinoAnalogReader final : public IAnalogReader {
public:
  explicit ArduinoAnalogReader(uint8_t resolutionBits = 10);

  void begin();

  int read(uint8_t pin) override;

  uint8_t resolutionBits() const;
  uint16_t adcMax() const;

private:
  uint8_t _resolutionBits;
  uint16_t _adcMax;
};
