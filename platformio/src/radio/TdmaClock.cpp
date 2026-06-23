// ---
// description: Implements TdmaClock's session clock, slot-index math, and myTurn() transmit gating.
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
