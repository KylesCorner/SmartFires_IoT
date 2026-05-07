// -----------------------------------------------------------------------------
// Wind Sensor Rev C
// -----------------------------------------------------------------------------
//
// Hardware:
//   TPS 5V boost EN  -> A3
//   Wind RV divider  -> A1
//   Wind TMP divider -> A2
//
// IMPORTANT:
//   Set WIND_DIVIDER_RATIO to your real resistor divider.
//
// Formula:
//   Wind output -> Rtop -> ADC pin -> Rbottom -> GND
//   WIND_DIVIDER_RATIO = (Rtop + Rbottom) / Rbottom

#pragma once

#include "interfaces/IAnalogReader.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"
#include "interfaces/Itps.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

class WindSensorRevC final : public ISensor {
public:
  struct Config {
    uint8_t pinRv = 1;
    uint8_t pinTmp = 2;

    float adcRefVolts = 3.3f;
    uint16_t adcMax = 1023;

    // Reconstructs the actual wind-sensor-side voltage from the divided ADC
    // voltage.
    //
    // dividerRatio = (Rtop + Rbottom) / Rbottom
    //
    // Example:
    //   Sensor output -> 10k -> ADC pin -> 22k -> GND
    //   dividerRatio = (10 + 22) / 22 = 1.4545
    float rvDividerRatio = 1.0f;
    float tmpDividerRatio = 1.0f;

    // Calibration offset used by the Modern Device Rev C algorithm.
    // Tune this with the wind sensor covered / no airflow until wind_mps ~= 0.
    float zeroWindAdjustmentVolts = 0.20f;

    // The published Rev C math is based on 5 V / 10-bit Arduino ADC units.
    // We measure voltage directly, then convert back into equivalent 5 V ADC
    // units before using that regression.
    float formulaRefVolts = 5.0f;
    uint16_t formulaAdcMax = 1023;

    uint32_t minSamplePeriodMs = 1000;
    uint32_t wakeDelayMs = 2000;

    SensorDutyClass dutyClass = SensorDutyClass::DutyCycled;

    static Config makeRevCCfg(uint8_t pinRv_ = 1,
                              uint8_t pinTmp_ = 2,
                              float adcRefVolts_ = 3.3f,
                              uint16_t adcMax_ = 1023,
                              float rvDividerRatio_ = 1.0f,
                              float tmpDividerRatio_ = 1.0f,
                              float zeroWindAdjustmentVolts_ = 0.20f,
                              uint32_t minSamplePeriodMs_ = 10,
                              uint32_t wakeDelayMs_ = 2000,
                              SensorDutyClass dutyClass_ =
                                  SensorDutyClass::DutyCycled) {
      Config cfg;
      cfg.pinRv = pinRv_;
      cfg.pinTmp = pinTmp_;
      cfg.adcRefVolts = adcRefVolts_;
      cfg.adcMax = adcMax_;
      cfg.rvDividerRatio = rvDividerRatio_;
      cfg.tmpDividerRatio = tmpDividerRatio_;
      cfg.zeroWindAdjustmentVolts = zeroWindAdjustmentVolts_;
      cfg.minSamplePeriodMs = minSamplePeriodMs_;
      cfg.wakeDelayMs = wakeDelayMs_;
      cfg.dutyClass = dutyClass_;
      return cfg;
    }
  };

  struct Reading {
    int rawRv = 0;
    int rawTmp = 0;

    float rvVolts = NAN;
    float tmpVolts = NAN;

    float tempC = NAN;
    float zeroWindVolts = NAN;

    float windMph = NAN;
    float windMps = NAN;

    bool valid = false;
    uint32_t timestampMs = 0;
  };

  WindSensorRevC(const Config &cfg,
                 IAnalogReader &analog,
                 Itps &power,
                 IClock &clock);

  const char *name() const override;

  bool begin() override;
  bool wake() override;
  bool sleep() override;
  bool service() override;
  bool sample() override;

  bool ready() const override;
  bool healthy() const override;

  SensorPowerState powerState() const override;
  SensorDutyClass dutyClass() const override;

  const Reading &reading() const;

  const void *readingData() const override;
  size_t readingSize() const override;

  size_t writeTelemetry(char *out, size_t maxLen) const override;
  void fillSnapshot(SensorSnapshot &snap) const override;

private:
  Config _cfg;
  IAnalogReader &_analog;
  Itps &_power;
  IClock &_clock;

  Reading _reading;

  bool _healthy = false;
  SensorPowerState _state = SensorPowerState::Off;

  uint32_t _wakeStartMs = 0;
  uint32_t _lastSampleMs = 0;
  bool _hasSampled = false;

  float adcToSensorVolts(int raw, float dividerRatio) const;
  float voltsToFormulaAdcUnits(float volts) const;

  float estimateTempC(float tmpVolts) const;
  float estimateZeroWindVolts(float tmpVolts) const;
  float estimateWindMph(float rvVolts, float zeroWindVolts) const;
};
