#include "platform/ArduinoAnalogReader.h"

#include <Arduino.h>

ArduinoAnalogReader::ArduinoAnalogReader(uint8_t resolutionBits)
    : _resolutionBits(resolutionBits),
      _adcMax(static_cast<uint16_t>((1UL << resolutionBits) - 1UL)) {}

void ArduinoAnalogReader::begin() {
#if defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_ESP32)
  analogReadResolution(_resolutionBits);
#endif
}

int ArduinoAnalogReader::read(uint8_t pin) {
  return analogRead(pin);
}

uint8_t ArduinoAnalogReader::resolutionBits() const {
  return _resolutionBits;
}

uint16_t ArduinoAnalogReader::adcMax() const {
  return _adcMax;
}
