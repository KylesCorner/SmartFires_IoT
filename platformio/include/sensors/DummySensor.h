#pragma once

#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"
#include "telemetry/SensorSnapshot.h"

#include <stdint.h>
#include <stdio.h>

// Synthetic sensor for DUMMY_SENSORS firmware builds.
// Fills wind, PM, GPS, and IMU snapshot fields with a triangle-wave oscillation
// driven by sample count so PacketHandler's delta encoding gets a real workout.
// Always-On duty class — begin() immediately sets state to Ready.
class DummySensor final : public ISensor {
public:
    explicit DummySensor(IClock &clock) : _clock(clock) {}

    const char *name() const override { return "DummySensor"; }

    bool begin() override {
        _healthy = true;
        _state   = SensorPowerState::Ready;
        return true;
    }

    bool wake()    override { return true; }
    bool sleep()   override { return true; }
    bool service() override { return _healthy; }

    bool sample() override {
        if (!_healthy) return false;
        _sampleCount++;
        return true;
    }

    bool ready()   const override { return _healthy && _state == SensorPowerState::Ready; }
    bool healthy() const override { return _healthy; }

    SensorPowerState powerState() const override { return _state; }
    SensorDutyClass  dutyClass()  const override { return SensorDutyClass::AlwaysOn; }

    const void *readingData() const override { return nullptr; }
    size_t      readingSize() const override { return 0; }

    size_t writeTelemetry(char *out, size_t maxLen) const override {
        if (!out || maxLen == 0) return 0;
        int n = snprintf(out, maxLen, "dummy,samples=%lu",
                         (unsigned long)_sampleCount);
        if (n < 0) return 0;
        return (size_t)n >= maxLen ? maxLen - 1 : (size_t)n;
    }

    void fillSnapshot(SensorSnapshot &snap) const override {
        // Triangle wave over kPeriod samples: t sweeps 0.0 -> 1.0 -> 0.0
        constexpr uint32_t kPeriod = 60;
        uint32_t phase = _sampleCount % kPeriod;
        float t = (phase < kPeriod / 2)
                      ? (float)phase / (kPeriod / 2)
                      : (float)(kPeriod - phase) / (kPeriod / 2);

        snap.sensorFlags |= 0x01 | 0x04 | 0x08 | 0x10; // WIND | GPS | IMU | SPS30

        snap.windMps = 0.5f  + 4.5f  * t;   // 0.5 – 5.0 m/s
        snap.pm1_0   = 5.0f  + 5.0f  * t;   // 5  – 10 µg/m³
        snap.pm2_5   = 8.0f  + 7.0f  * t;   // 8  – 15 µg/m³
        snap.pm4_0   = 10.0f + 8.0f  * t;   // 10 – 18 µg/m³
        snap.pm10    = 12.0f + 10.0f * t;   // 12 – 22 µg/m³

        // Static GPS fix — Yosemite Valley (plausible wildfire location)
        snap.latDeg  = 37.7456f;
        snap.lonDeg  = -119.5936f;
    }

private:
    IClock          &_clock;
    SensorPowerState _state       = SensorPowerState::Off;
    bool             _healthy     = false;
    uint32_t         _sampleCount = 0;
};
