#pragma once

#include "DroneContext.h"

class SensorManager {
public:
  explicit SensorManager(DroneContext &ctx) : _ctx(ctx) {}

  void beginAll();
  void sampleAll();
  void sampleKeypadOnly();

  void sleepAllSensors();

  void startWakeSequence();
  void serviceWakeSequence();

  bool priorityWarmupComplete() const;
  bool allSensorsReady() const;
  bool wakeSequenceActive() const;
  bool wakeSequenceComplete() const;

  void printSensorReadings();
  void printNotReadySensors() const;
  bool sht31DeltaTriggered(float tempThresholdC, float humidityThresholdPct);

private:
  enum class WakePhase : uint8_t {
    Idle = 0,
    WaitingPriorityWarmup,
    WaitingAllReady,
    Complete
  };

  bool isPriorityWarmupSensor_(size_t index) const;
  bool wakeSensor_(size_t index, const char *label);

private:
  DroneContext &_ctx;
  WakePhase _wakePhase = WakePhase::Idle;

  bool _sht31BaselineValid = false;
  float _lastSht31TempC = 0.0f;
  float _lastSht31HumidityPct = 0.0f;
};
