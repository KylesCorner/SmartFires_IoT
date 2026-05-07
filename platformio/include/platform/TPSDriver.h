#pragma once

#include "interfaces/Itps.h"

#include <stdint.h>

class TPSDriver final : public Itps{
public:
  struct Config {
    uint8_t enablePin = 0;
    bool activeHigh = true;

    static Config make(uint8_t enablePin_, bool activeHigh_ = true) {
      Config cfg;
      cfg.enablePin = enablePin_;
      cfg.activeHigh = activeHigh_;
      return cfg;
    }
  };

  explicit TPSDriver (const Config &cfg);

  bool begin() override;
  bool enable() override;
  bool disable() override;
  bool enabled() const override;

private:
  Config _cfg;
  bool _enabled = false;

  void writeEnabled(bool on);
};
