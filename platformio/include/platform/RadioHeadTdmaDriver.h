#pragma once

#include "radio/ITdmaRadioDriver.h"
#include <Arduino.h>
#include <RHReliableDatagram.h>
#include <RH_RF95.h>

class RadioHeadTdmaDriver final : public ITdmaRadioDriver {
public:
  struct Config {
    uint8_t address;
    uint8_t csPin;
    uint8_t intPin;
    uint8_t rstPin;
    float frequencyMhz;
    int8_t txPowerDbm;
    uint8_t retries;
    uint16_t timeoutMs;
    uint16_t cadTimeoutMs;

    static RadioHeadTdmaDriver::Config
    radioHeadCfg(uint8_t address_ = 1, uint8_t csPin_ = 8, uint8_t intPin_ = 3,
                 uint8_t rstPin_ = 4, float frequencyMhz_ = 915.0f,
                 int8_t txPowerDbm_ = 13, uint8_t retries_ = 1,
                 uint16_t timeoutMs_ = 100, uint16_t cadTimeoutMs_ = 10) {

      RadioHeadTdmaDriver::Config cfg;
      cfg.address = address_;
      cfg.csPin = csPin_;
      cfg.intPin = intPin_;
      cfg.rstPin = rstPin_;
      cfg.frequencyMhz = frequencyMhz_;
      cfg.txPowerDbm = txPowerDbm_;
      cfg.retries = retries_;
      cfg.timeoutMs = timeoutMs_;
      cfg.cadTimeoutMs = cadTimeoutMs_;
      return cfg;
    }
  };

  explicit RadioHeadTdmaDriver(const Config &cfg);

  bool begin() override;
  bool sendToWait(const uint8_t *data, uint8_t len, uint8_t to) override;
  bool available() override;
  bool receive(ReceivedPacket &out) override;
  bool healthy() const override;

private:
  Config _cfg;
  RH_RF95 _rf95;
  RHReliableDatagram _manager;
  bool _healthy = false;

  void resetRadio();
};
