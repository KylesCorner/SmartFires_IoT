#include <Arduino.h>

#if defined(LORA_BASE)

#include "app/SmartFiresBaseApp.h"
#include "platform/ArduinoClock.h"
#include "platform/RadioHeadTdmaDriver.h"

ArduinoClock baseClock;

RadioHeadTdmaDriver::Config baseRadioCfg =
    RadioHeadTdmaDriver::Config::radioHeadCfg(0x01);
RadioHeadTdmaDriver baseRadio(baseRadioCfg);

SmartFiresBaseApp::Config baseAppCfg = SmartFiresBaseApp::Config::baseCfg();
SmartFiresBaseApp baseApp(baseAppCfg, baseClock, baseRadio, Serial1, Serial);

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  Serial.println("SmartFires base station starting...");
  if (!baseApp.begin()) {
    Serial.println("SmartFires base app begin failed");
    while (true) {
      delay(500);
    }
  }
  Serial.println("SmartFires base app ready");
}

void loop() {
  baseApp.update();
  delay(5);
}

#elif defined(LORA_NODE)

#include <Wire.h>

#include "app/SmartFiresNodeApp.h"
#include "platform/BoardIdentify.h"

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

#include "platform/AdafruitSht31Driver.h"
#include "platform/AdafruitGpsDriver.h"
#include "platform/SparkfunIcm20948Driver.h"
#include "platform/SensirionUartSps30Driver.h"

#include "sensors/Icm20948Sensor.h"
#include "sensors/Sht31Sensor.h"
#include "sensors/Pa1010dGpsSensor.h"
#include "sensors/Sps30Sensor.h"

// #ifndef nodeId
// #define nodeId 1
// #endif

#ifndef NUM_SLOTS
#define NUM_SLOTS 2
#endif

namespace {

TdmaConfig makeNodeTdmaCfg(uint8_t nodeId, uint8_t numSlots) {
  TdmaConfig cfg = TdmaConfig::tdmaCfg(nodeId, 0x01, numSlots);
  cfg.enableLinkAck = true;
  cfg.maxRetries = 3;
  cfg.ackTimeoutMs = 250;
  return cfg;
}

RadioHeadTdmaDriver::Config makeNodeRadioCfg(uint8_t nodeId,
                                             uint16_t ackTimeoutMs) {
  RadioHeadTdmaDriver::Config cfg =
      RadioHeadTdmaDriver::Config::radioHeadCfg(nodeId);
  cfg.timeoutMs = ackTimeoutMs;
  return cfg;
}

}  // namespace

// -----------------------------------------------------------------------------
// Platform
// -----------------------------------------------------------------------------
ArduinoClock clock;
ArduinoAnalogReader analog;

// -----------------------------------------------------------------------------
// Sensors
// -----------------------------------------------------------------------------

AdafruitSht31Driver sht31Driver;

// makeSht31Cfg(uint8_t address_ = 0x45, uint32_t minSamplesPeriodMs_ = 1000,
//              uint32_t wakeDelayMs_ = 15,
//              SensorDutyClass dutyClass_ = SensorDutyClass::DutyCycled) {

Sht31Sensor::Config sht31Cfg =
    Sht31Sensor::Config::makeSht31Cfg(0x45, 100, 0, SensorDutyClass::AlwaysOn);
Sht31Sensor sht31(sht31Cfg, sht31Driver, clock);

AdafruitGpsDriver gpsDriver;
Pa1010dGpsSensor::Config gpsCfg = Pa1010dGpsSensor::Config::makeGpsCfg();
Pa1010dGpsSensor gps(gpsCfg,gpsDriver,clock);

SparkfunIcm20948Driver imuDriver;
Icm20948Sensor::Config imuCfg = Icm20948Sensor::Config::makeImuCfg();
Icm20948Sensor imu(imuCfg, imuDriver, clock);

Sps30Sensor::Config sps30Cfg = Sps30Sensor::Config::makeSps30Cfg();
SensirionUartSps30Driver sps30Driver(Serial1);
Sps30Sensor sps30(sps30Cfg, sps30Driver, clock);

ISensor* sensors[] = {
    &sht31,
    &gps,
    &imu,
    &sps30,
};

constexpr size_t sensorCount = sizeof(sensors) / sizeof(sensors[0]);

// -----------------------------------------------------------------------------
// Battery
// -----------------------------------------------------------------------------

BatteryMonitor::Config batteryCfg = BatteryMonitor::Config::makeBatConfig();
BatteryMonitor battery(batteryCfg, analog, clock);

// -----------------------------------------------------------------------------
// Duty Cycle 
// -----------------------------------------------------------------------------

DutyCycleConfig dutyCfg = DutyCycleConfig::dutyCycleCfg();
DutyCycleController duty(dutyCfg, sht31, sensors, sensorCount, clock);

// -----------------------------------------------------------------------------
// Networking
// -----------------------------------------------------------------------------

constexpr uint8_t numSlots = NUM_SLOTS;
const uint8_t nodeId = BoardIdentity::smallId(1, numSlots);

PacketHandler::Config packetHandlerCfg = PacketHandler::Config::make(nodeId);
PacketHandler packetHandler(packetHandlerCfg);

TdmaConfig tdmaCfg = makeNodeTdmaCfg(nodeId, numSlots);
TdmaClock tdmaClock(tdmaCfg, clock);
TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);

RadioHeadTdmaDriver::Config radioDriverCfg =
  makeNodeRadioCfg(nodeId, tdmaCfg.ackTimeoutMs);
RadioHeadTdmaDriver radioDriver(radioDriverCfg);

TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

SmartFiresNodeApp::Config appCfg =
  SmartFiresNodeApp::Config::appCfg(nodeId, false, false);

// -----------------------------------------------------------------------------
// App
// -----------------------------------------------------------------------------

SmartFiresNodeApp app(appCfg, clock, duty, packetHandler, tdmaRadio, tdmaClock,
                      sensors, sensorCount, &battery);

void scanI2C() {
  Serial.println("I2C scan...");
  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("Found device at 0x");
      if (addr < 16) {
        Serial.print('0');
      }
      Serial.println(addr, HEX);
    }
  }
}

void setup() {
  delay(1000);
  Serial.begin(115200);
  Serial1.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  Wire.begin();
  scanI2C();

  // if (!sps30.begin()) {
  //   Serial.println("SPS begin FAILED");
  //   while (true) {
  //     delay(1000);
  //   }
  // }
  //
  // Serial.println("SPS begin OK");
  //
  // if (!sps30.wake()) {
  //   Serial.println("SPS wake FAILED");
  // }

  Serial.println("SmartFires Feather TDMA node starting...");
  Serial.print("Board ID: ");
  Serial.println(nodeId);
  Serial.print("MY_SLOT: ");
  Serial.println((nodeId - 1) % numSlots);
  Serial.print("NUM_SLOTS: ");
  Serial.println(numSlots);
  Serial.print("SLOT_WIDTH: ");
  Serial.print(tdmaCfg.slotWidthMs);
  Serial.println(" ms");
  Serial.print("GUARD: ");
  Serial.print(tdmaCfg.guardMs);
  Serial.println(" ms");
  Serial.print("SYNC_STALE: ");
  Serial.print(tdmaCfg.syncStaleMs / 1000);
  Serial.println(" s");
  Serial.print("APP_RELIAB: ");
  Serial.println(tdmaCfg.enableAppReliability ? "ON" : "OFF");
  Serial.print("LINK_ACK: ");
  Serial.println(tdmaCfg.enableLinkAck ? "WAIT_FOR_ACK" : "FIRE_AND_FORGET");
  Serial.print("RETX_WINDOW: ");
  Serial.println(tdmaCfg.reliabilityWindowDepth);
  Serial.print("RETX_MAX_ATT: ");
  Serial.println(tdmaCfg.reliabilityMaxAttempts);
  Serial.print("LINK_RETRIES: ");
  Serial.println(tdmaCfg.maxRetries);
  Serial.print("ACK_TIMEOUT: ");
  Serial.print(tdmaCfg.ackTimeoutMs);
  Serial.println(" ms");

  if (!app.begin()) {
    Serial.println("SmartFires app begin failed");
    while (true) {
      delay(500);
    }
  }

  Serial.println("SmartFires app ready");
}

void loop() {
  // sps30.service();
  //
  // if (sps30.ready()) {
  //   if (sps30.sample()) {
  //     char buf[180];
  //     sps30.writeTelemetry(buf, sizeof(buf));
  //     Serial.println(buf);
  //   } else {
  //     Serial.println("SPS sample failed");
  //   }
  // }
  //
  // delay(1000);
  app.update();
  delay(25);
}

#else
#error "Define exactly one firmware role: LORA_BASE or LORA_NODE"
#endif
