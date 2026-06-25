// ---
// description: GPIO enable-pin driver implementing Itps to power-gate the wind sensor's TPS regulator.
// role: implementation
// ---
#include "platform/TPSDriver.h"

#if defined(ARDUINO)

#include <Arduino.h>

TPSDriver::TPSDriver(const Config &cfg)
    : _cfg(cfg) {}

bool TPSDriver::begin() {
  pinMode(_cfg.enablePin, OUTPUT);

  // Safe default: TPS disabled until the wind sensor is explicitly woken.
  writeEnabled(false);
  return true;
}

bool TPSDriver::enable() {
  writeEnabled(true);
  return true;
}

bool TPSDriver::disable() {
  writeEnabled(false);
  return true;
}

bool TPSDriver::enabled() const {
  return _enabled;
}

void TPSDriver::writeEnabled(bool on) {
  _enabled = on;

  const uint8_t level = (_cfg.activeHigh == on) ? HIGH : LOW;
  digitalWrite(_cfg.enablePin, level);
}

#else

TPSDriver::TPSDriver(const Config &cfg)
    : _cfg(cfg) {}

bool TPSDriver::begin() {
  _enabled = false;
  return true;
}

bool TPSDriver::enable() {
  _enabled = true;
  return true;
}

bool TPSDriver::disable() {
  _enabled = false;
  return true;
}

bool TPSDriver::enabled() const {
  return _enabled;
}

void TPSDriver::writeEnabled(bool on) {
  _enabled = on;
}

#endif
