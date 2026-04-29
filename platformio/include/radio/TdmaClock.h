#pragma once

#include "interfaces/IClock.h"
#include "radio/TdmaConfig.h"

#include <stdint.h>

class TdmaClock {
public:
  explicit TdmaClock(const TdmaConfig &cfg, IClock &clock);

  void applySync(uint32_t sessionTimeMs);
  bool hasSync() const;
  bool syncStale() const;

  uint32_t sessionNowMs() const;
  uint32_t currentSlotIndex() const;
  uint8_t currentSlotNumber() const;
  uint32_t positionInSlotMs() const;

  uint8_t mySlot() const;

  bool myTurn(uint32_t &slotIndexOut) const;

private:
  TdmaConfig _cfg;
  IClock &_clock;

  uint32_t _syncSessionMs = 0;
  uint32_t _syncLocalMs = 0;
  bool _hasSync = false;
};
