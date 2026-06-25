// ---
// description: Tracks the TDMA session clock and slot timing, deriving whose turn it is to transmit.
// role: implementation
// ---
#pragma once

#include "interfaces/IClock.h"
#include "radio/TdmaConfig.h"

#include <stdint.h>

class TdmaClock {
public:
  explicit TdmaClock(const TdmaConfig &cfg, IClock &clock);

  void applyAssignment(uint8_t nodeId, uint8_t numSlots);
  void applySync(uint32_t sessionId, uint32_t sessionTimeMs);
  bool hasSync() const;
  bool syncStale() const;
  bool consumeSessionChanged();

  uint32_t sessionNowMs() const;
  uint32_t currentSlotIndex() const;
  uint8_t currentSlotNumber() const;
  uint32_t positionInSlotMs() const;

  uint8_t mySlot() const;

  bool myTurn(uint32_t &slotIndexOut) const;

  // True while it's safe to listen for base-originated traffic (TIME_SYNC,
  // ACK_SUMMARY, CMD_CALIBRATE/RESET) — i.e. slot 0, the base's permanently
  // reserved slot, the only slot the base ever transmits in. Also true
  // during the last rxWakeAheadMs of the prior slot, so the radio is already
  // listening before the base's earliest possible transmit rather than
  // racing to wake exactly at the slot-0 boundary. Also true unconditionally
  // before first sync or once sync goes stale, since slot timing can't be
  // trusted yet/anymore: callers should fall back to continuous receive in
  // both cases, mirroring myTurn()'s own fallback.
  bool baseRxWindowOpen() const;

private:
  TdmaConfig _cfg;
  IClock &_clock;

  uint32_t _syncSessionMs = 0;
  uint32_t _syncLocalMs = 0;
  uint32_t _sessionId = 0;
  bool _hasSync = false;
  bool _sessionChanged = false;
};
