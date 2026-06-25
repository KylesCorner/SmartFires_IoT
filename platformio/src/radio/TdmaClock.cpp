// ---
// description: Implements TdmaClock's session clock, slot-index math, myTurn() transmit gating, and baseRxWindowOpen() receive gating.
// role: implementation
// docs: [tdma-protocol]
// ---
#include "radio/TdmaClock.h"

TdmaClock::TdmaClock(const TdmaConfig &cfg, IClock &clock)
    : _cfg(cfg), _clock(clock) {}

void TdmaClock::applyAssignment(uint8_t nodeId, uint8_t numSlots) {
  _cfg.nodeId = nodeId;
  _cfg.numSlots = numSlots;
}

void TdmaClock::applySync(uint32_t sessionId, uint32_t sessionTimeMs) {
  if (sessionId != _sessionId) {
    _sessionId      = sessionId;
    _sessionChanged = true;
  }
  _syncSessionMs = sessionTimeMs;
  _syncLocalMs   = _clock.millis();
  _hasSync       = true;
}

bool TdmaClock::consumeSessionChanged() {
  if (!_sessionChanged) return false;
  _sessionChanged = false;
  return true;
}

bool TdmaClock::hasSync() const {
  return _hasSync;
}

bool TdmaClock::syncStale() const {
  if (!_hasSync) {
    return false;
  }

  return (_clock.millis() - _syncLocalMs) > _cfg.syncStaleMs;
}

uint32_t TdmaClock::sessionNowMs() const {
  if (!_hasSync) {
    return _clock.millis();
  }

  return _syncSessionMs + (_clock.millis() - _syncLocalMs);
}

uint32_t TdmaClock::currentSlotIndex() const {
  return sessionNowMs() / _cfg.slotWidthMs;
}

uint8_t TdmaClock::currentSlotNumber() const {
  return static_cast<uint8_t>(currentSlotIndex() % _cfg.numSlots);
}

uint32_t TdmaClock::positionInSlotMs() const {
  return sessionNowMs() % _cfg.slotWidthMs;
}

uint8_t TdmaClock::mySlot() const {
  if (_cfg.nodeId == 0 || _cfg.numSlots == 0) {
    return 0xFF;
  }

  return static_cast<uint8_t>((_cfg.nodeId - 1) % _cfg.numSlots);
}

bool TdmaClock::myTurn(uint32_t &slotIndexOut) const {
  const uint32_t localMs = _clock.millis();

  if (!_hasSync || syncStale()) {
    slotIndexOut = localMs / _cfg.slotWidthMs;
    return true;
  }

  const uint32_t sessionMs = sessionNowMs();
  const uint32_t slotIndex = sessionMs / _cfg.slotWidthMs;
  const uint32_t posInSlot = sessionMs % _cfg.slotWidthMs;
  const uint8_t whichSlot = static_cast<uint8_t>(slotIndex % _cfg.numSlots);
  const uint8_t mySlotNumber = mySlot();

  slotIndexOut = slotIndex;

  if (mySlotNumber == 0xFF || whichSlot != mySlotNumber) {
    return false;
  }

  if (posInSlot < _cfg.guardMs) {
    return false;
  }

  if (posInSlot >= _cfg.slotWidthMs - _cfg.guardMs) {
    return false;
  }

  return true;
}

bool TdmaClock::baseRxWindowOpen() const {
  if (!_hasSync || syncStale()) {
    return true;
  }

  // Unlike myTurn(), no guard-band exclusion here: a receiver listening a
  // little longer than strictly necessary is harmless, whereas a transmitter
  // running past its guard band risks colliding with the next slot's owner.
  const uint8_t whichSlot = currentSlotNumber();
  if (whichSlot == 0) {
    return true;
  }

  // Wake-ahead: start listening during the tail of the prior slot (the last
  // slot in the frame, since slot 0 is the first), rather than racing to
  // notice the slot-0 boundary on the same loop tick it arrives.
  if (_cfg.numSlots > 0 && whichSlot == static_cast<uint8_t>(_cfg.numSlots - 1) &&
      positionInSlotMs() >= _cfg.slotWidthMs - _cfg.rxWakeAheadMs) {
    return true;
  }

  return false;
}
