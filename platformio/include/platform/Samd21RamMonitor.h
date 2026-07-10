#pragma once

#include <stdint.h>

class Samd21RamMonitor {
public:
  enum class State : uint8_t {
    Ok,
    Warning,
    Critical,
    Collision,
    Invalid,
  };

  struct Config {
    uint32_t samplePeriodMs = 1000;
    uint32_t logPeriodMs = 5000;
    uint32_t warnFreeBytes = 6U * 1024U;
    uint32_t criticalFreeBytes = 3U * 1024U;
    uint32_t newLowLogStepBytes = 256;

    static Config make(
        uint32_t samplePeriodMs_,
        uint32_t logPeriodMs_,
        uint32_t warnFreeBytes_,
        uint32_t criticalFreeBytes_,
        uint32_t newLowLogStepBytes_) {
      Config cfg;

      cfg.samplePeriodMs = samplePeriodMs_;
      cfg.logPeriodMs = logPeriodMs_;
      cfg.warnFreeBytes = warnFreeBytes_;
      cfg.criticalFreeBytes = criticalFreeBytes_;
      cfg.newLowLogStepBytes = newLowLogStepBytes_;

      return cfg;
    }
  };

  struct Snapshot {
    uint32_t timestampMs = 0;

    // Current raw addresses.
    uintptr_t stackPtr = 0;
    uintptr_t heapEnd = 0;

    // Current gap between heap and stack.
    uint32_t freeGapBytes = 0;

    // Historical low/high water marks.
    uint32_t minFreeGapBytes = 0;
    uintptr_t lowestStackPtr = 0;
    uintptr_t highestHeapEnd = 0;

    // Changes relative to first valid observation.
    uint32_t heapGrowthBytes = 0;
    uint32_t observedStackDropBytes = 0;

    State state = State::Invalid;
    bool valid = false;
  };

  explicit Samd21RamMonitor(const Config &cfg);

  // Initialize history and emit the first measurement.
  void begin();

  // Periodic sampling and event-driven logging.
  void update();

  // Force an immediate sample and log around an important operation.
  void checkpoint(const char *reason, const char *subject = nullptr);

  const Snapshot &snapshot() const;

  // For the future hardware-watchdog policy.
  bool critical() const;

private:
  const Snapshot &sampleNow();

  void logSnapshot(
      const Snapshot &snapshot,
      const char *reason,
      const char *subject);

  static const char *stateName(State state);

  Config _cfg;
  Snapshot _latest;

  bool _begun = false;
  bool _historyInitialized = false;

  uintptr_t _initialStackPtr = 0;
  uintptr_t _initialHeapEnd = 0;

  uintptr_t _lowestStackPtr = 0;
  uintptr_t _highestHeapEnd = 0;
  uint32_t _minFreeGapBytes = 0;

  uint32_t _lastSampleMs = 0;
  uint32_t _lastLogMs = 0;

  uint32_t _lastLoggedMinFreeBytes = UINT32_MAX;
  State _lastLoggedState = State::Invalid;
};

#if defined(LORA_NODE)
extern Samd21RamMonitor gRamMonitor;
#endif