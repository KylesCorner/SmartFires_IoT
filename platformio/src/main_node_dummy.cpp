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
#define NUM_SLOTS 2
#endif

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

PacketHandler::Config packetHandlerCfg = PacketHandler::Config::make(NODE_ID);
PacketHandler packetHandler(packetHandlerCfg);

TdmaConfig tdmaCfg = TdmaConfig::tdmaCfg(
    NODE_ID,
    0x01,
    NUM_SLOTS,
    900,
    20,
    1320000,
    4,
    1,
    100,
    false);
TdmaClock tdmaClock(tdmaCfg, clock);
TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);

RadioHeadTdmaDriver::Config radioDriverCfg =
    RadioHeadTdmaDriver::Config::radioHeadCfg(NODE_ID);
RadioHeadTdmaDriver radioDriver(radioDriverCfg);

TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

SmartFiresNodeApp::Config appCfg =
    SmartFiresNodeApp::Config::appCfg(NODE_ID, false);

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
    Serial.print("[DUMMY] NODE_ID      = "); Serial.println(NODE_ID);
    Serial.print("[DUMMY] MY_SLOT      = "); Serial.println((NODE_ID - 1) % NUM_SLOTS);
    Serial.print("[DUMMY] NUM_SLOTS    = "); Serial.println(NUM_SLOTS);
    Serial.print("[DUMMY] SLOT_WIDTH   = "); Serial.print(tdmaCfg.slotWidthMs); Serial.println(" ms");
    Serial.print("[DUMMY] GUARD        = "); Serial.print(tdmaCfg.guardMs);      Serial.println(" ms");
    Serial.print("[DUMMY] SYNC_STALE   = "); Serial.print(tdmaCfg.syncStaleMs / 1000); Serial.println(" s");
    Serial.print("[DUMMY] APP_RELIAB   = "); Serial.println(tdmaCfg.enableAppReliability ? "ON" : "OFF");
    Serial.println("[DUMMY] LINK_ACK     = ON (send succeeds only if base acks)");
    Serial.print("[DUMMY] RETX_WINDOW  = "); Serial.println(tdmaCfg.reliabilityWindowDepth);
    Serial.print("[DUMMY] RETX_MAX_ATT = "); Serial.println(tdmaCfg.reliabilityMaxAttempts);
    Serial.println("========================================");

    if (!app.begin()) {
        Serial.println("[DUMMY] app begin failed");
        while (true) { delay(500); }
    }

    Serial.println("[DUMMY] app ready  --  transmitting synthetic data");
}

void loop() {
    app.update();
    delay(25);
}
