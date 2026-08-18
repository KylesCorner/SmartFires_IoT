// ---
// description: IClock over the Arduino core's own millis(), used by the always-awake builds (base station, power test) that never enter MCU standby.
// role: implementation
// ---
#pragma once

#include "interfaces/IClock.h"

#include <stdint.h>

// Nodes duty-cycle into standby, where the core's tick source stops, so they
// use Samd21RtcClock instead. This clock used to carry a _sleepOffsetMs that
// Samd21RtcSleep patched in after every standby; that whole mechanism is gone
// now that the node's timebase is the free-running RTC counter itself.
class ArduinoClock final : public IClock {
public:
  uint32_t millis() const override;
};
