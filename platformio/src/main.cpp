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
#include "sensors/Sht31Sensor.h"

#ifndef NODE_ID
#define NODE_ID 1
#endif

#ifndef NUM_SLOTS
#define NUM_SLOTS 2
#endif

ArduinoClock clock;
ArduinoAnalogReader analog;

AdafruitSht31Driver sht31Driver;
Sht31Sensor::Config sht31Cfg =
    Sht31Sensor::Config::makeSht31Cfg(0x45, 1000, 0, SensorDutyClass::AlwaysOn);
Sht31Sensor sht31(sht31Cfg, sht31Driver, clock);

ISensor* sensors[] = {
    &sht31,
};

constexpr size_t sensorCount = 1;

BatteryMonitor::Config batteryCfg = BatteryMonitor::Config::makeBatConfig();
BatteryMonitor battery(batteryCfg, analog, clock);

DutyCycleConfig dutyCfg = DutyCycleConfig::dutyCycleCfg();
DutyCycleController duty(dutyCfg, sht31, sensors, sensorCount, clock);

PacketHandler::Config packetHandlerCfg = PacketHandler::Config::make(NODE_ID);
PacketHandler packetHandler(packetHandlerCfg);

TdmaConfig tdmaCfg = TdmaConfig::tdmaCfg();
TdmaClock tdmaClock(tdmaCfg, clock);
TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);

RadioHeadTdmaDriver::Config radioDriverCfg =
    RadioHeadTdmaDriver::Config::radioHeadCfg(NODE_ID);
RadioHeadTdmaDriver radioDriver(radioDriverCfg);

TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

SmartFiresNodeApp::Config appCfg =
    SmartFiresNodeApp::Config::appCfg(NODE_ID, false);

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
  while (!Serial && millis() < 3000) {
  }

  Wire.begin();
  scanI2C();

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
  app.update();
  delay(25);
}

#endif
