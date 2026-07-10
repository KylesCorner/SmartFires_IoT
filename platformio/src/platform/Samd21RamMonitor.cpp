#include "platform/Samd21RamMonitor.h"

#include "logging/DebugLogger.h"

#include <Arduino.h>
#include <unistd.h>

namespace {

// Read the actual Cortex-M main stack pointer.
//
// The Feather M0 uses the Cortex-M0+ main stack in normal firmware execution.
uintptr_t readStackPointer() {
  uintptr_t sp = 0;

  __asm volatile(
      "mov %0, sp"
      : "=r"(sp));

  return sp;
}

// sbrk(0) does not allocate memory.
// It returns the current program break: the upper boundary of the heap.
uintptr_t readHeapEnd() {
  void *currentBreak = sbrk(0);

  if (currentBreak ==
      reinterpret_cast<void *>(static_cast<intptr_t>(-1))) {
    return 0;
  }

  return reinterpret_cast<uintptr_t>(currentBreak);
}

} // namespace

Samd21RamMonitor::Samd21RamMonitor(const Config &cfg)
    : _cfg(cfg) {}

void Samd21RamMonitor::begin() {
  _begun = true;
  _historyInitialized = false;

  _initialStackPtr = 0;
  _initialHeapEnd = 0;

  _lowestStackPtr = 0;
  _highestHeapEnd = 0;
  _minFreeGapBytes = 0;

  _lastSampleMs = 0;
  _lastLogMs = 0;

  _lastLoggedMinFreeBytes = UINT32_MAX;
  _lastLoggedState = State::Invalid;

  const Snapshot &snap = sampleNow();

  logSnapshot(snap, "begin", nullptr);
}

void Samd21RamMonitor::update() {
  if (!_begun) {
    begin();
    return;
  }

  const uint32_t now = millis();

  if (now - _lastSampleMs < _cfg.samplePeriodMs) {
    return;
  }

  const Snapshot &snap = sampleNow();

  const bool stateChanged =
      snap.state != _lastLoggedState;

  const bool periodicLogDue =
      snap.timestampMs - _lastLogMs >= _cfg.logPeriodMs;

  bool meaningfulNewLow = false;

  if (snap.valid &&
      _lastLoggedMinFreeBytes != UINT32_MAX &&
      snap.minFreeGapBytes < _lastLoggedMinFreeBytes) {

    const uint32_t drop =
        _lastLoggedMinFreeBytes - snap.minFreeGapBytes;

    meaningfulNewLow =
        drop >= _cfg.newLowLogStepBytes;
  }

  if (stateChanged ||
      periodicLogDue ||
      meaningfulNewLow) {
    logSnapshot(snap, "periodic", nullptr);
  }
}

void Samd21RamMonitor::checkpoint(
    const char *reason,
    const char *subject) {

  if (!_begun) {
    begin();
  }

  const Snapshot &snap = sampleNow();

  logSnapshot(snap, reason, subject);
}

const Samd21RamMonitor::Snapshot &
Samd21RamMonitor::sampleNow() {
  Snapshot next;

  /*
   * IMPORTANT:
   *
   * Capture the raw addresses before doing any logging.
   *
   * DebugLogger::logf() allocates its formatted message buffer on the stack,
   * so measuring after logging would contaminate the measurement.
   */
  next.timestampMs = millis();
  next.stackPtr = readStackPointer();
  next.heapEnd = readHeapEnd();

  next.valid =
      next.stackPtr != 0 &&
      next.heapEnd != 0;

  if (!next.valid) {
    next.state = State::Invalid;

    _latest = next;
    _lastSampleMs = next.timestampMs;

    return _latest;
  }

  if (next.stackPtr <= next.heapEnd) {
    // The heap has reached or crossed the current stack position.
    next.freeGapBytes = 0;
    next.state = State::Collision;
  } else {
    next.freeGapBytes =
        static_cast<uint32_t>(
            next.stackPtr - next.heapEnd);

    if (next.freeGapBytes <= _cfg.criticalFreeBytes) {
      next.state = State::Critical;
    } else if (next.freeGapBytes <= _cfg.warnFreeBytes) {
      next.state = State::Warning;
    } else {
      next.state = State::Ok;
    }
  }

  if (!_historyInitialized) {
    _historyInitialized = true;

    _initialStackPtr = next.stackPtr;
    _initialHeapEnd = next.heapEnd;

    _lowestStackPtr = next.stackPtr;
    _highestHeapEnd = next.heapEnd;
    _minFreeGapBytes = next.freeGapBytes;
  } else {
    if (next.stackPtr < _lowestStackPtr) {
      _lowestStackPtr = next.stackPtr;
    }

    if (next.heapEnd > _highestHeapEnd) {
      _highestHeapEnd = next.heapEnd;
    }

    if (next.freeGapBytes < _minFreeGapBytes) {
      _minFreeGapBytes = next.freeGapBytes;
    }
  }

  next.lowestStackPtr = _lowestStackPtr;
  next.highestHeapEnd = _highestHeapEnd;
  next.minFreeGapBytes = _minFreeGapBytes;

  if (_highestHeapEnd > _initialHeapEnd) {
    next.heapGrowthBytes =
        static_cast<uint32_t>(
            _highestHeapEnd - _initialHeapEnd);
  }

  if (_initialStackPtr > _lowestStackPtr) {
    next.observedStackDropBytes =
        static_cast<uint32_t>(
            _initialStackPtr - _lowestStackPtr);
  }

  _latest = next;
  _lastSampleMs = next.timestampMs;

  return _latest;
}

void Samd21RamMonitor::logSnapshot(
    const Snapshot &snap,
    const char *reason,
    const char *subject) {

  const char *why =
      reason != nullptr ? reason : "-";

  const char *object =
      subject != nullptr ? subject : "-";

  LogLevel level = LogLevel::Debug;

  switch (snap.state) {
  case State::Warning:
    level = LogLevel::Warn;
    break;

  case State::Critical:
  case State::Collision:
  case State::Invalid:
    level = LogLevel::Error;
    break;

  case State::Ok:
  default:
    level = LogLevel::Debug;
    break;
  }

  gLog.logf(
      level,
      "ram",
      "why=%s obj=%s sp=%08lX hp=%08lX free=%lu min=%lu hg=%lu sd=%lu state=%s",
      why,
      object,
      static_cast<unsigned long>(snap.stackPtr),
      static_cast<unsigned long>(snap.heapEnd),
      static_cast<unsigned long>(snap.freeGapBytes),
      static_cast<unsigned long>(snap.minFreeGapBytes),
      static_cast<unsigned long>(snap.heapGrowthBytes),
      static_cast<unsigned long>(snap.observedStackDropBytes),
      stateName(snap.state));

  _lastLogMs = snap.timestampMs;
  _lastLoggedMinFreeBytes = snap.minFreeGapBytes;
  _lastLoggedState = snap.state;
}

const Samd21RamMonitor::Snapshot &
Samd21RamMonitor::snapshot() const {
  return _latest;
}

bool Samd21RamMonitor::critical() const {
  return
      _latest.state == State::Critical ||
      _latest.state == State::Collision ||
      _latest.state == State::Invalid;
}

const char *
Samd21RamMonitor::stateName(State state) {
  switch (state) {
  case State::Ok:
    return "OK";

  case State::Warning:
    return "WARN";

  case State::Critical:
    return "CRIT";

  case State::Collision:
    return "COLLIDE";

  case State::Invalid:
  default:
    return "INVALID";
  }
}