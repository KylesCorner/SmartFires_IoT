#pragma once

#include "config/NetworkConfig.h"
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

    // Thin wrapper: every field besides `address` (which varies per board,
    // assigned from uid_hash) comes straight from config/NetworkConfig.h.
    // `retries`/`timeoutMs` are the same NetworkConfig::kLinkRetries /
    // kLinkAckTimeoutMs constants that TdmaConfig::maxRetries/ackTimeoutMs
    // use, so the two can no longer drift apart the way they used to when
    // main.cpp threaded one into the other by hand.
    static RadioHeadTdmaDriver::Config radioHeadCfg(uint8_t address_) {
      RadioHeadTdmaDriver::Config cfg;
      cfg.address = address_;
      cfg.csPin = NetworkConfig::kRadioCsPin;
      cfg.intPin = NetworkConfig::kRadioIntPin;
      cfg.rstPin = NetworkConfig::kRadioRstPin;
      cfg.frequencyMhz = NetworkConfig::kRadioFrequencyMhz;
      cfg.txPowerDbm = NetworkConfig::kRadioTxPowerDbm;
      cfg.retries = NetworkConfig::kLinkRetries;
      cfg.timeoutMs = NetworkConfig::kLinkAckTimeoutMs;
      cfg.cadTimeoutMs = NetworkConfig::kRadioCadTimeoutMs;
      return cfg;
    }
  };

  explicit RadioHeadTdmaDriver(const Config &cfg);

  bool begin() override;
  bool send(const uint8_t *data, uint8_t len, uint8_t to) override;
  bool sendToWait(const uint8_t *data, uint8_t len, uint8_t to) override;
  bool setLocalAddress(uint8_t address) override;
  bool available() override;
  bool receive(ReceivedPacket &out, bool autoAck = true) override;
  void acknowledge(uint8_t from, uint8_t id) override;
  bool healthy() const override;

private:
  Config _cfg;
  RH_RF95 _rf95;
  RHReliableDatagram _manager;
  bool _healthy = false;
  uint8_t _nextDatagramId = 0;

  void resetRadio();
};
