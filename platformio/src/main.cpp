#include <Arduino.h>
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

// Sensors/drivers here
#include "platform/AdafruitSht31Driver.h"
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

AdafruitSht31Driver sht31Driver;

    // makeSht31Cfg(uint8_t address_ = 0x45, uint32_t minSamplesPeriodMs_ = 1000,
    //              uint32_t wakeDelayMs_ = 15,
    //              SensorDutyClass dutyClass_ = SensorDutyClass::DutyCycled) {

Sht31Sensor::Config sht31Cfg = Sht31Sensor::Config::makeSht31Cfg(0x45,1000,0,SensorDutyClass::AlwaysOn);
Sht31Sensor sht31(sht31Cfg, sht31Driver, clock);

ISensor *sensors[] = {
    &sht31,
};

// constexpr size_t sensorCount = sizeof(sensors) / sizeof(sensors[0]);
constexpr size_t sensorCount = 1;

// -----------------------------------------------------------------------------
// Battery
// -----------------------------------------------------------------------------

BatteryMonitor::Config batteryCfg = BatteryMonitor::Config::makeBatConfig();
BatteryMonitor battery(batteryCfg, analog, clock);

// -----------------------------------------------------------------------------
// Duty cycle
// -----------------------------------------------------------------------------

DutyCycleConfig dutyCfg = DutyCycleConfig::dutyCycleCfg();
DutyCycleController duty(dutyCfg,sht31, sensors, sensorCount, clock);

// -----------------------------------------------------------------------------
// Telemetry
// -----------------------------------------------------------------------------

PacketHandler::Config packetHandlerCfg = PacketHandler::Config::make(NODE_ID);
PacketHandler packetHandler(packetHandlerCfg);

// -----------------------------------------------------------------------------
// TDMA LoRa
// -----------------------------------------------------------------------------

TdmaConfig tdmaCfg = TdmaConfig::tdmaCfg();
TdmaClock tdmaClock(tdmaCfg, clock);
TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);

RadioHeadTdmaDriver::Config radioDriverCfg =
    RadioHeadTdmaDriver::Config::radioHeadCfg(NODE_ID);
RadioHeadTdmaDriver radioDriver(radioDriverCfg);

TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

// -----------------------------------------------------------------------------
// App
// -----------------------------------------------------------------------------

// SmartFiresNodeApp::Config appCfg;
// appCfg.enableBattery = true;
SmartFiresNodeApp::Config appCfg = SmartFiresNodeApp::Config::appCfg(NODE_ID, false);

SmartFiresNodeApp app(appCfg, clock, duty, packetHandler, tdmaRadio, tdmaClock,
                      sensors, sensorCount, &battery);

void scanI2C() {
  Serial.println("I2C scan...");
  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("Found device at 0x");
      if (addr < 16)
        Serial.print('0');
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
  // static uint32_t lastPrintMs = 0;
  //
  // sht31.service();
  //
  // if (sht31.ready()) {
  //   if (sht31.sample()) {
  //     const auto &r = sht31.reading();
  //
  //     Serial.print("[SHT31] temp_c=");
  //     Serial.print(r.tempC, 2);
  //     Serial.print(" humidity_pct=");
  //     Serial.print(r.humidityPct, 2);
  //     Serial.print(" valid=");
  //     Serial.print(r.valid ? 1 : 0);
  //     Serial.print(" t_ms=");
  //     Serial.println(r.timestampMs);
  //   } else {
  //     Serial.println("[SHT31] sample failed");
  //   }
  // }

  delay(25);
}
