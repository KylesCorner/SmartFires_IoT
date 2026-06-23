// ---
// description: Plain data struct holding one sensor reading cycle in natural float units, filled by ISensor::fillSnapshot() and quantized to wire format by PacketHandler.
// role: implementation
// ---
#pragma once

#include <math.h>
#include <stdint.h>

// Internal representation of one sensor reading cycle.
// Fields are in natural float units — PacketHandler quantizes them to wire format.
// Sensors fill their own fields via ISensor::fillSnapshot().

struct SensorSnapshot {
    uint32_t sessionTimeMs = 0;   // from TdmaClock::sessionNowMs()

    uint16_t sensorFlags   = 0;  // WIND=0x01 SHT31=0x02 GPS=0x04 IMU=0x08 SPS30=0x10

    float windMps          = -1.0f;
    float tempC            = -1.0f;
    float humidityPct      = -1.0f;
    float pm1_0            = -1.0f;
    float pm2_5            = -1.0f;
    float pm4_0            = -1.0f;
    float pm10             = -1.0f;

    float latDeg           = 0.0f;
    float lonDeg           = 0.0f;

    // Battery — populated by SmartFiresNodeApp from BatteryMonitor if enabled.
    uint16_t batteryMv     = -1;
    uint8_t  batteryPct    = -1;
    bool     batteryValid  = false;

    // IMU fields — DMP 9DOF heading, populated by Icm20948Sensor::fillSnapshot().
    float    headingDeg      = -1.0f;  // degrees 0–360; -1 = invalid
    uint16_t headingAccuracy = 0;      // Q12 raw; divide by 4096 for degrees
    bool     imuValid        = false;
};
