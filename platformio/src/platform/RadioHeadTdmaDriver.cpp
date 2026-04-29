#include "platform/RadioHeadTdmaDriver.h"

#include <Arduino.h>

RadioHeadTdmaDriver::RadioHeadTdmaDriver(const Config &cfg)
    : _cfg(cfg),
      _rf95(cfg.csPin, cfg.intPin),
      _manager(_rf95, cfg.address) {}

void RadioHeadTdmaDriver::resetRadio() {
  pinMode(_cfg.rstPin, OUTPUT);

  digitalWrite(_cfg.rstPin, HIGH);
  delay(10);

  digitalWrite(_cfg.rstPin, LOW);
  delay(10);

  digitalWrite(_cfg.rstPin, HIGH);
  delay(10);
}

bool RadioHeadTdmaDriver::begin() {
  resetRadio();

  if (!_manager.init()) {
    _healthy = false;
    return false;
  }

  if (!_rf95.setFrequency(_cfg.frequencyMhz)) {
    _healthy = false;
    return false;
  }

  _rf95.setTxPower(_cfg.txPowerDbm, false);
  _manager.setRetries(_cfg.retries);
  _manager.setTimeout(_cfg.timeoutMs);
  _rf95.setCADTimeout(_cfg.cadTimeoutMs);

  _healthy = true;
  return true;
}

bool RadioHeadTdmaDriver::send(const uint8_t *data,
                               uint8_t len,
                               uint8_t to) {
  if (!_healthy || !data || len == 0) {
    return false;
  }

  return _manager.sendto(const_cast<uint8_t *>(data), len, to);
}

bool RadioHeadTdmaDriver::sendToWait(const uint8_t *data,
                                     uint8_t len,
                                     uint8_t to) {
  if (!_healthy || !data || len == 0) {
    return false;
  }

  return _manager.sendtoWait(const_cast<uint8_t *>(data), len, to);
}

bool RadioHeadTdmaDriver::available() {
  return _healthy && _manager.available();
}

bool RadioHeadTdmaDriver::receive(ReceivedPacket &out) {
  if (!_healthy) {
    return false;
  }

  uint8_t len = sizeof(out.data);
  uint8_t from = 0;

  if (!_manager.recvfromAck(out.data, &len, &from)) {
    return false;
  }

  out.from = from;
  out.len = len;
  out.rssi = static_cast<int8_t>(_rf95.lastRssi());

  return true;
}

bool RadioHeadTdmaDriver::healthy() const {
  return _healthy;
}
