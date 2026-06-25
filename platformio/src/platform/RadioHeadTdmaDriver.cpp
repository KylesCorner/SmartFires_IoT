// ---
// description: RHReliableDatagram/RH_RF95-based LoRa radio driver implementing ITdmaRadioDriver for the TDMA stack.
// role: implementation
// docs: [tdma-protocol]
// ---
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
  // Temporarily disable explicit CAD timeout while debugging missed packets
  // between node and base; leave RadioHead on its default behavior.
  // _rf95.setCADTimeout(_cfg.cadTimeoutMs);

  _healthy = true;
  return true;
}

bool RadioHeadTdmaDriver::send(const uint8_t *data,
                               uint8_t len,
                               uint8_t to) {
  if (!_healthy || !data || len == 0) {
    return false;
  }

  _manager.setHeaderId(++_nextDatagramId);
  _manager.setHeaderFlags(RH_FLAGS_NONE, RH_FLAGS_ACK | RH_FLAGS_RETRY);
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

bool RadioHeadTdmaDriver::setLocalAddress(uint8_t address) {
  if (!_healthy) {
    return false;
  }

  _cfg.address = address;
  _manager.setThisAddress(address);
  return true;
}

bool RadioHeadTdmaDriver::available() {
  return _healthy && _manager.available();
}

bool RadioHeadTdmaDriver::sleep() {
  if (!_healthy) {
    return false;
  }

  // RH_RF95::sleep() just writes the SLEEP opmode register; the next
  // available()/send()/sendToWait() call re-arms whatever mode it needs
  // regardless of current state, so there is no separate "wake" call to make.
  return _rf95.sleep();
}

bool RadioHeadTdmaDriver::receive(ReceivedPacket &out, bool autoAck) {
  if (!_healthy) {
    return false;
  }

  uint8_t len = sizeof(out.data);
  uint8_t from = 0;
  uint8_t id = 0;

  // recvfromAck() unconditionally ACKs any accepted unicast datagram — it has
  // no notion of "the sender didn't ask for one". recvfrom() (inherited from
  // RHDatagram) skips that ACK entirely, at the cost of also skipping RH's
  // duplicate-id rejection; callers that need an ACK call acknowledge() once
  // they've decoded enough of the payload to know it's warranted.
  const bool ok = autoAck ? _manager.recvfromAck(out.data, &len, &from, nullptr, &id)
                          : _manager.recvfrom(out.data, &len, &from, nullptr, &id);
  if (!ok) {
    return false;
  }

  out.from = from;
  out.id = id;
  out.len = len;
  out.rssi = static_cast<int8_t>(_rf95.lastRssi());

  return true;
}

void RadioHeadTdmaDriver::acknowledge(uint8_t from, uint8_t id) {
  if (!_healthy) {
    return;
  }

  // RHReliableDatagram::acknowledge() does exactly this, but it's protected,
  // so we can't call it directly through `_manager` — mirror it here. The
  // 1-byte (not 0-byte) ack body matches RH's own workaround for radios that
  // wedge on a zero-length CRC-error packet; see RHReliableDatagram.cpp.
  _manager.setHeaderId(id);
  _manager.setHeaderFlags(RH_FLAGS_ACK);
  uint8_t ack = '!';
  _manager.sendto(&ack, sizeof(ack), from);
}

bool RadioHeadTdmaDriver::healthy() const {
  return _healthy;
}
