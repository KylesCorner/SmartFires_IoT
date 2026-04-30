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

#else

#include <Wire.h>

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

#include "platform/AdafruitSht31Driver.h"
#include "platform/AdafruitGpsDriver.h"
#include "platform/SparkfunIcm20948Driver.h"
#include "platform/SensirionUartSps30Driver.h"

#include "sensors/Icm20948Sensor.h"
#include "sensors/Sht31Sensor.h"
#include "sensors/Pa1010dGpsSensor.h"
#include "sensors/Sps30Sensor.h"

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

PacketHandler::Config packetHandlerCfg = PacketHandler::Config::make(NODE_ID);
PacketHandler packetHandler(packetHandlerCfg);

TdmaConfig tdmaCfg = TdmaConfig::tdmaCfg(NODE_ID, 0x01, NUM_SLOTS);
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

#endif
