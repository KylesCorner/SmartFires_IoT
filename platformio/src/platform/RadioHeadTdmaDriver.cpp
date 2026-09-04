// ---
// description: RHReliableDatagram/RH_RF95-based LoRa radio driver implementing ITdmaRadioDriver for the TDMA stack.
// role: implementation
// docs: [lora-vs-lorawan, packet-reliability, tdma-protocol]
// ---
#include "platform/RadioHeadTdmaDriver.h"

#include "config/NetworkConfig.h"
#include "logging/DebugLogger.h"
#include "platform/ResetDiagnostics.h"

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

  _healthy = true;
  return true;
}

bool RadioHeadTdmaDriver::setTxPower(int8_t dbm) {
  if (!_healthy) {
    return false;
  }

  // Written back into _cfg, not just pushed at the radio, so begin()'s
  // unconditional re-apply above cannot silently revert a commanded level on a
  // radio reinit. A real MCU reboot rebuilds Config from NetworkConfig and so
  // still lands on the static baseline — the intended fail-safe.
  _cfg.txPowerDbm = dbm;

  // useRFO=false selects PA_BOOST, matching begin(). The two must agree: the
  // same dBm number means different hardware settings on the RFO pin, and the
  // module's antenna is wired to PA_BOOST.
  _rf95.setTxPower(dbm, false);
  return true;
}

int8_t RadioHeadTdmaDriver::txPowerDbm() const { return _cfg.txPowerDbm; }

bool RadioHeadTdmaDriver::send(const uint8_t *data,
                               uint8_t len,
                               uint8_t to) {
  if (!_healthy || !data || len == 0) {
    return false;
  }

  // Radio TX (sendto + waitPacketSent) is the prime WDT-hang suspect — RH_RF95's
  // leading no-timeout waitPacketSent() can spin on a missed TX-done IRQ. Mark
  // the zone so such a hang is attributed to ZONE_RADIO_TX (see ResetDiagnostics).
  ResetDiagnostics::ZoneScope zone(ResetDiagnostics::ZONE_RADIO_TX);

  _manager.setHeaderId(++_nextDatagramId);
  _manager.setHeaderFlags(RH_FLAGS_NONE, RH_FLAGS_ACK | RH_FLAGS_RETRY);
  const bool queued = _manager.sendto(const_cast<uint8_t *>(data), len, to);

  // Bounded wait for the transmission to physically finish, for the same
  // reason as acknowledge(): without it, nothing stops a subsequent sleep()
  // (or any other radio call) from aborting this send mid-flight, and
  // nothing stops the *next* send()/sendToWait() call from hanging on
  // RH_RF95::send()'s own no-timeout leading waitPacketSent() (it refuses to
  // start a new transmission while it still believes one is in progress). A
  // timeout here doesn't necessarily mean the packet was lost — the SX1276
  // may well have finished transmitting despite a missed TX-done interrupt —
  // so it's logged, not treated as send failure; `queued` (whether RadioHead
  // accepted the packet for transmission at all) is the return value.
  if (queued && !_manager.waitPacketSent(NetworkConfig::kSendTxWaitMs)) {
    LOG_WARN("radio", "send_tx_timeout to=%u len=%u timeout_ms=%u",
             static_cast<unsigned int>(to), static_cast<unsigned int>(len),
             static_cast<unsigned int>(NetworkConfig::kSendTxWaitMs));
  }

  return queued;
}

bool RadioHeadTdmaDriver::sendToWait(const uint8_t *data,
                                     uint8_t len,
                                     uint8_t to) {
  if (!_healthy || !data || len == 0) {
    return false;
  }

  // sendtoWait() blocks on RadioHead's own no-timeout waitPacketSent() inside
  // RH_RF95::send() — the AWAKEN handshake path. Attributed to ZONE_RADIO_TX.
  ResetDiagnostics::ZoneScope zone(ResetDiagnostics::ZONE_RADIO_TX);

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
  uint8_t to = 0;
  uint8_t id = 0;

  // recvfromAck() unconditionally ACKs any accepted unicast datagram — it has
  // no notion of "the sender didn't ask for one", and does so by blocking on
  // RadioHead's own no-timeout waitPacketSent() (see ITdmaRadioDriver.h).
  // recvfrom() (inherited from RHDatagram) skips that ACK entirely, at the
  // cost of also skipping RH's duplicate-id rejection; callers that need an
  // ACK call acknowledge() once they've decoded enough of the payload to
  // know it's warranted, and once they've confirmed `to` isn't a broadcast.
  const bool ok = autoAck ? _manager.recvfromAck(out.data, &len, &from, &to, &id)
                          : _manager.recvfrom(out.data, &len, &from, &to, &id);
  if (!ok) {
    return false;
  }

  out.from = from;
  out.to = to;
  out.id = id;
  out.len = len;
  out.rssi = static_cast<int8_t>(_rf95.lastRssi());
  // RH_RF95::lastSNR() already returns whole dB (it divides the raw register
  // value by 4 internally), so no scaling here.
  out.snr = static_cast<int8_t>(_rf95.lastSNR());

  return true;
}

void RadioHeadTdmaDriver::acknowledge(uint8_t from, uint8_t id) {
  if (!_healthy) {
    return;
  }

  // Sends an ACK then waits (bounded) for it to finish transmitting — still a
  // radio-TX blocking region, so attributed to ZONE_RADIO_TX.
  ResetDiagnostics::ZoneScope zone(ResetDiagnostics::ZONE_RADIO_TX);

  // RHReliableDatagram::acknowledge() does exactly this, but it's protected,
  // so we can't call it directly through `_manager` — mirror it here. The
  // 1-byte (not 0-byte) ack body matches RH's own workaround for radios that
  // wedge on a zero-length CRC-error packet; see RHReliableDatagram.cpp.
  _manager.setHeaderId(id);
  _manager.setHeaderFlags(RH_FLAGS_ACK);
  uint8_t ack = '!';
  _manager.sendto(&ack, sizeof(ack), from);

  // Bounded wait for the ACK to physically finish transmitting, so nothing
  // downstream (notably TdmaRadioService::updateRxPower()'s sleep() call,
  // which has no in-flight-TX guard) can abort it mid-send. Bounded, not
  // absent, so a missed TX-done interrupt can't hang the board the way
  // RadioHead's own no-arg waitPacketSent() can — see
  // ITdmaRadioDriver::acknowledge()'s comment.
  if (!_manager.waitPacketSent(NetworkConfig::kAckTxWaitMs)) {
    LOG_WARN("radio", "ack_tx_timeout from=%u id=%u timeout_ms=%u",
             static_cast<unsigned int>(from), static_cast<unsigned int>(id),
             static_cast<unsigned int>(NetworkConfig::kAckTxWaitMs));
  }
}

bool RadioHeadTdmaDriver::healthy() const {
  return _healthy;
}
