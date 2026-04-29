#pragma once

#include <math.h>
#include <stdint.h>

// Internal representation of one sensor reading cycle.
// Fields are in natural float units — PacketHandler quantizes them to wire format.
// Sensors fill their own fields via ISensor::fillSnapshot().

struct SensorSnapshot {
    uint32_t sessionTimeMs = 0;   // from TdmaClock::sessionNowMs()

    uint16_t sensorFlags   = 0;  // WIND=0x01 SHT31=0x02 GPS=0x04 IMU=0x08 SPS30=0x10

    float windMps          = 0.0f;
    float tempC            = NAN;
    float humidityPct      = NAN;
    float pm1_0            = 0.0f;
    float pm2_5            = 0.0f;
    float pm4_0            = 0.0f;
    float pm10             = 0.0f;

    float latDeg           = 0.0f;
    float lonDeg           = 0.0f;

    // Battery — populated by SmartFiresNodeApp from BatteryMonitor if enabled.
    uint16_t batteryMv     = 0;
    uint8_t  batteryPct    = 0;
    bool     batteryValid  = false;
};
