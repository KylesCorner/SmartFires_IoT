// Dummy-sensor node entry point.
// Identical wiring to main.cpp (node path) but substitutes DummySht31Driver
// and DummySensor for all real hardware.  Flash with:
//   pio run -e feather_m0_lora_node_dummy --target upload
// The base station and Jetson pipeline behave exactly as with a real node.

#include <Arduino.h>

#include "app/SmartFiresNodeApp.h"

#include "interfaces/ISensor.h"
#include "platform/ArduinoAnalogReader.h"
#include "platform/ArduinoClock.h"
#include "platform/RadioHeadTdmaDriver.h"

#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"

#include "radio/PacketHandler.h"
#include "radio/TdmaClock.h"
#include "radio/TdmaConfig.h"
#include "radio/TdmaRadioService.h"
#include "radio/TdmaTxQueue.h"

#include "platform/DummySht31Driver.h"
#include "sensors/DummySensor.h"
#include "sensors/Sht31Sensor.h"

#ifndef NODE_ID
#define NODE_ID 1
#endif

#ifndef NUM_SLOTS
#define NUM_SLOTS 4
#endif

#ifndef SMARTFIRES_TDMA_RELIABILITY_MODE
#define SMARTFIRES_TDMA_RELIABILITY_MODE 0
#endif

namespace {

constexpr uint8_t kBaseRadioAddr = 0x01;
constexpr uint8_t kUnassignedNodeId = 0x00;

TdmaReliabilityMode telemetryReliabilityMode() {
    return tdmaReliabilityModeFromValue(SMARTFIRES_TDMA_RELIABILITY_MODE);
}

const char *reliabilityModeName(TdmaReliabilityMode mode) {
    switch (mode) {
        case TdmaReliabilityMode::StrictLinkAck:
            return "STRICT_LINK_ACK";
        case TdmaReliabilityMode::AppLayerAckSummary:
            return "APP_ACK_SUMMARY";
    }

    return "UNKNOWN";
}

uint8_t makeInitialRadioAddr(uint32_t uidHash) {
    uint8_t addr = static_cast<uint8_t>(0x80u | (uidHash & 0x3Fu));
    if (addr == 0xFFu) {
        addr = 0x80u;
    }
    return addr;
}

constexpr uint32_t makeDummyUidHash() {
    return 0xD00D0000UL | static_cast<uint32_t>(NODE_ID);
}

TdmaConfig makeDummyTdmaCfg() {
    TdmaConfig cfg = TdmaConfig::tdmaCfg(kUnassignedNodeId, kBaseRadioAddr, NUM_SLOTS);
    cfg.reliabilityMode = telemetryReliabilityMode();
    // Toggle normal telemetry between sendToWait(true) and fire-and-forget(false).
    cfg.enableLinkAck = (cfg.reliabilityMode == TdmaReliabilityMode::StrictLinkAck);
    // Keep retries visible in logs during packet-transmission isolation.
    cfg.maxRetries = 3;
    cfg.ackTimeoutMs = 250;
    cfg.queueDepth = 8;
    cfg.reliabilityWindowDepth = 8;
    cfg.reliabilityMaxAgeMs = 30000;
    cfg.expectedAckIntervalMs = 4000;
    cfg.retryWaitMultiplierPermille = 2000;
    cfg.retryWaitMinMs = 4500;
    cfg.retryWaitMaxMs = 10000;
    cfg.requireAckSummaryBeforeFirstRetry = false;
    return cfg;
}

RadioHeadTdmaDriver::Config makeDummyRadioCfg(uint8_t radioAddr,
                          uint16_t ackTimeoutMs) {
    RadioHeadTdmaDriver::Config cfg =
        RadioHeadTdmaDriver::Config::radioHeadCfg(radioAddr);
    // Disable RadioHead internal retry loop so TdmaRadioService logs each ack attempt.
    cfg.retries = 0;
    cfg.timeoutMs = ackTimeoutMs;
    return cfg;
}

} // namespace

// -----------------------------------------------------------------------------
// Platform
// -----------------------------------------------------------------------------
ArduinoClock clock;
ArduinoAnalogReader analog;

// -----------------------------------------------------------------------------
// Sensors
// -----------------------------------------------------------------------------

// DummySht31Driver satisfies DutyCycleController's Sht31Sensor& requirement
// without any real hardware.
DummySht31Driver sht31Driver;
Sht31Sensor::Config sht31Cfg =
    Sht31Sensor::Config::makeSht31Cfg(0x45, 100, 0, SensorDutyClass::AlwaysOn);
Sht31Sensor sht31(sht31Cfg, sht31Driver, clock);

// DummySensor fills wind, PM, GPS, and IMU snapshot fields with a slowly
// drifting triangle wave so delta encoding gets a real workout.
DummySensor dummySensor(clock);

ISensor* sensors[] = { &sht31, &dummySensor };
constexpr size_t sensorCount = sizeof(sensors) / sizeof(sensors[0]);

// -----------------------------------------------------------------------------
// Battery
// -----------------------------------------------------------------------------

BatteryMonitor::Config batteryCfg = BatteryMonitor::Config::makeBatConfig();
BatteryMonitor battery(batteryCfg, analog, clock);

// -----------------------------------------------------------------------------
// Duty cycle
// -----------------------------------------------------------------------------

// Keep the dummy node streaming for long monitor sessions. The dummy SHT31
// trigger reading is intentionally static, so the default duty-cycle config
// would sample once, go back to sleep, and never cross the wake threshold
// again.
DutyCycleConfig dutyCfg = DutyCycleConfig::dutyCycleCfg(
    3000,        // minSleepMs
    1000,        // maxWakeMs
    86400000UL,  // activeSampleMs (24 h)
    500,         // samplePeriodMs
    250,         // warmupMs
    1.0f,        // tempDeltaThresholdC
    5.0f,        // humidityDeltaThresholdPct
    false);
DutyCycleController duty(dutyCfg, sht31, sensors, sensorCount, clock);

// -----------------------------------------------------------------------------
// Networking
// -----------------------------------------------------------------------------

constexpr uint32_t dummyUidHash = makeDummyUidHash();
const uint8_t initialRadioAddr = makeInitialRadioAddr(dummyUidHash);

PacketHandler::Config packetHandlerCfg =
    PacketHandler::Config::make(kUnassignedNodeId);
PacketHandler packetHandler(packetHandlerCfg);

TdmaConfig tdmaCfg = makeDummyTdmaCfg();
TdmaClock tdmaClock(tdmaCfg, clock);
TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);

RadioHeadTdmaDriver::Config radioDriverCfg =
    makeDummyRadioCfg(initialRadioAddr, tdmaCfg.ackTimeoutMs);
RadioHeadTdmaDriver radioDriver(radioDriverCfg);

TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

SmartFiresNodeApp::Config appCfg =
//set third value here to true for awaken only mode in dummy mode
    SmartFiresNodeApp::Config::appCfg(kUnassignedNodeId, dummyUidHash, false, false);

// -----------------------------------------------------------------------------
// App
// -----------------------------------------------------------------------------

SmartFiresNodeApp app(appCfg, clock, duty, packetHandler, tdmaRadio, tdmaClock,
                      sensors, sensorCount, &battery);

void setup() {
    delay(1000);
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {}

    Serial.println("========================================");
    Serial.println("[DUMMY] SmartFires node  --  synthetic data mode");
    Serial.println("========================================");
    Serial.print("[DUMMY] UID_HASH     = 0x"); Serial.println(dummyUidHash, HEX);
    Serial.print("[DUMMY] RADIO_INIT   = "); Serial.println(initialRadioAddr);
    Serial.print("[DUMMY] TDMA_ENTS    = "); Serial.println(NUM_SLOTS);
    Serial.print("[DUMMY] SLOT_WIDTH   = "); Serial.print(tdmaCfg.slotWidthMs); Serial.println(" ms");
    Serial.print("[DUMMY] GUARD        = "); Serial.print(tdmaCfg.guardMs);      Serial.println(" ms");
    Serial.print("[DUMMY] SYNC_STALE   = "); Serial.print(tdmaCfg.syncStaleMs / 1000); Serial.println(" s");
    Serial.print("[DUMMY] APP_RELIAB   = "); Serial.println(tdmaCfg.enableAppReliability ? "ON" : "OFF");
    Serial.print("[DUMMY] LINK_ACK     = "); Serial.println(tdmaCfg.enableLinkAck ? "WAIT_FOR_ACK" : "FIRE_AND_FORGET");
    Serial.print("[DUMMY] QUEUE_DEPTH  = "); Serial.println(tdmaCfg.queueDepth);
    Serial.print("[DUMMY] RETX_WINDOW  = "); Serial.println(tdmaCfg.reliabilityWindowDepth);
    Serial.print("[DUMMY] RETX_MAX_ATT = "); Serial.println(tdmaCfg.reliabilityMaxAttempts);
    Serial.print("[DUMMY] RETX_MAX_AGE = "); Serial.print(tdmaCfg.reliabilityMaxAgeMs); Serial.println(" ms");
    Serial.print("[DUMMY] LINK_RETRIES = "); Serial.println(tdmaCfg.maxRetries);
    Serial.print("[DUMMY] ACK_TIMEOUT  = "); Serial.print(tdmaCfg.ackTimeoutMs); Serial.println(" ms");
    Serial.print("[DUMMY] TELEM_MODE   = "); Serial.println(reliabilityModeName(tdmaCfg.reliabilityMode));
    Serial.println("[DUMMY] --- ACK-paced retry gate ---");
    Serial.print("[DUMMY] ACK_INTERVAL = "); Serial.print(tdmaCfg.expectedAckIntervalMs); Serial.println(" ms");
    Serial.print("[DUMMY] RETRY_MUL    = "); Serial.print(tdmaCfg.retryWaitMultiplierPermille); Serial.println("/1000");
    Serial.print("[DUMMY] RETRY_MIN    = "); Serial.print(tdmaCfg.retryWaitMinMs); Serial.println(" ms");
    Serial.print("[DUMMY] RETRY_MAX    = "); Serial.print(tdmaCfg.retryWaitMaxMs); Serial.println(" ms");
    Serial.print("[DUMMY] REQ_ACK_1ST  = "); Serial.println(tdmaCfg.requireAckSummaryBeforeFirstRetry ? "YES" : "NO");
    Serial.println("========================================");

    if (!app.begin()) {
        Serial.println("[DUMMY] app begin failed");
        while (true) { delay(500); }
    }

    if (appCfg.awakenOnlyMode) {
        Serial.println("[DUMMY] app ready  --  AWAKEN-only troubleshooting mode");
    } else {
        Serial.println("[DUMMY] app ready  --  normal telemetry mode after TIME_SYNC");
    }
}

void loop() {
    app.update();
    delay(25);
}
