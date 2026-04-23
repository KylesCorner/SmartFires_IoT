// IWarmup.h
#pragma once

class IWarmup {
public:
  virtual ~IWarmup() = default;

  // True if this sensor should be part of the staged "phase 1" wake.
  virtual bool requiresPriorityWarmup() const = 0;

  // True once the sensor's long warmup period has completed.
  virtual bool warmupComplete() const = 0;
};
