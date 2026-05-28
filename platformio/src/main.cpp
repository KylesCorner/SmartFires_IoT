#include "logging/DebugLogger.h"
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
DebugLogger gLog(Serial, baseAppCfg.baseAddr);

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  gLog.setMinLevel(LogLevel::Debug);

  LOG_INFO("boot", "SmartFires base station starting");
  if (!baseApp.begin()) {
    LOG_ERROR("boot", "SmartFires base app begin failed");
    while (true) {
      delay(500);
    }
  }
  LOG_INFO("boot", "SmartFires base app ready");
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

#include "platform/AdafruitGpsDriver.h"
#include "platform/AdafruitSht31Driver.h"
#include "platform/SensirionUartSps30Driver.h"
#include "platform/SparkfunIcm20948Driver.h"
#include "platform/TPSDriver.h"

#include "sensors/Icm20948Sensor.h"
#include "sensors/Pa1010dGpsSensor.h"
#include "sensors/Sht31Sensor.h"
#include "sensors/Sps30Sensor.h"
#include "sensors/WindSensorRevC.h"

// #ifndef nodeId
// #define nodeId 1
// #endif

#ifndef NUM_SLOTS
#define NUM_SLOTS 4
#endif

#ifndef SMARTFIRES_TDMA_RELIABILITY_MODE
#define SMARTFIRES_TDMA_RELIABILITY_MODE 0
#endif

#ifndef SMARTFIRES_STATUS_INTERVAL_MS
#define SMARTFIRES_STATUS_INTERVAL_MS (15UL * 60UL * 1000UL)
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

TdmaConfig makeNodeTdmaCfg(uint8_t numSlots) {
  TdmaConfig cfg =
      TdmaConfig::tdmaCfg(kUnassignedNodeId, kBaseRadioAddr, numSlots);
  cfg.reliabilityMode = telemetryReliabilityMode();
  cfg.enableLinkAck =
      (cfg.reliabilityMode == TdmaReliabilityMode::StrictLinkAck);
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

RadioHeadTdmaDriver::Config makeNodeRadioCfg(uint8_t radioAddr,
                                             uint16_t ackTimeoutMs) {
  RadioHeadTdmaDriver::Config cfg =
      RadioHeadTdmaDriver::Config::radioHeadCfg(radioAddr);
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

constexpr uint8_t PIN_WIND_RV = A1;
constexpr uint8_t PIN_WIND_TMP = A2;
constexpr uint8_t PIN_WIND_ENABLE = A3;

constexpr float WIND_DIVIDER_RATIO = 1.6818f;

TPSDriver::Config windPowerCfg = TPSDriver::Config::make(PIN_WIND_ENABLE, true);

TPSDriver windPower(windPowerCfg);

WindSensorRevC::Config windCfg = WindSensorRevC::Config::makeRevCCfg(
    PIN_WIND_RV, PIN_WIND_TMP,
    3.3f,               // Feather ADC reference voltage
    1023,               // 10-bit ADC by default
    WIND_DIVIDER_RATIO, // RV divider reconstruction
    WIND_DIVIDER_RATIO, // TMP divider reconstruction
    -1.0f,              // zero-wind adjustment, calibrate later
    10,                 // minSamplePeriodMs
    10000,              // wakeDelayMs for hot-wire/TPS settling
    SensorDutyClass::DutyCycled);

WindSensorRevC wind(windCfg, analog, windPower, clock);

AdafruitSht31Driver sht31Driver;

Sht31Sensor::Config sht31Cfg = Sht31Sensor::Config::makeSht31Cfg();
Sht31Sensor sht31(sht31Cfg, sht31Driver, clock);

AdafruitGpsDriver gpsDriver;
Pa1010dGpsSensor::Config gpsCfg =
    Pa1010dGpsSensor::Config::makePeriodicBackupCfg();
Pa1010dGpsSensor gps(gpsCfg, gpsDriver, clock);

SparkfunIcm20948Driver imuDriver;
Icm20948Sensor::Config imuCfg = Icm20948Sensor::Config::makeImuCfg();
Icm20948Sensor imu(imuCfg, imuDriver, clock);

Sps30Sensor::Config sps30Cfg = Sps30Sensor::Config::makeSps30Cfg();
SensirionUartSps30Driver sps30Driver(Serial1);
Sps30Sensor sps30(sps30Cfg, sps30Driver, clock);

ISensor *sensors[] = {
    &sht31, &gps, &imu, &sps30, &wind,
};

// ISensor *sensors[] = {
//     &sht31, &imu,
// };
constexpr size_t sensorCount = sizeof(sensors) / sizeof(sensors[0]);

// -----------------------------------------------------------------------------
// Battery
// -----------------------------------------------------------------------------

BatteryMonitor::Config batteryCfg = BatteryMonitor::Config::makeBatConfig();
BatteryMonitor battery(batteryCfg, analog, clock);

// -----------------------------------------------------------------------------
// Duty Cycle
// -----------------------------------------------------------------------------

DutyCycleConfig dutyCfg = DutyCycleConfig::dutyCycleCfgContinuous();
DutyCycleController duty(dutyCfg, sht31, sensors, sensorCount, clock, battery);

// -----------------------------------------------------------------------------
// Networking
// -----------------------------------------------------------------------------

constexpr uint8_t numSlots = NUM_SLOTS;
const uint32_t nodeUidHash = BoardIdentity::hash32();
const uint8_t initialRadioAddr = makeInitialRadioAddr(nodeUidHash);

PacketHandler::Config packetHandlerCfg = PacketHandler::Config::make(
    kUnassignedNodeId, BinaryPacket::kBundleMaxDeltas,
    SMARTFIRES_STATUS_INTERVAL_MS);
PacketHandler packetHandler(packetHandlerCfg);

TdmaConfig tdmaCfg = makeNodeTdmaCfg(numSlots);
TdmaClock tdmaClock(tdmaCfg, clock);
TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);

RadioHeadTdmaDriver::Config radioDriverCfg =
    makeNodeRadioCfg(initialRadioAddr, tdmaCfg.ackTimeoutMs);
RadioHeadTdmaDriver radioDriver(radioDriverCfg);

TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

SmartFiresNodeApp::Config appCfg = SmartFiresNodeApp::Config::appCfg(
  kUnassignedNodeId, nodeUidHash, true, false,
  false); // set false to keep STATUS and skip bundle encoding/transmit

// -----------------------------------------------------------------------------
// App
// -----------------------------------------------------------------------------

SmartFiresNodeApp app(appCfg, clock, duty, packetHandler, tdmaRadio, tdmaClock,
                      sensors, sensorCount, &battery);

DebugLogger gLog(Serial, initialRadioAddr);

void scanI2C() {
  LOG_INFO("i2c", "scan_start");

  uint8_t foundCount = 0;

  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();

    if (err == 0) {
      ++foundCount;
      LOG_INFO("i2c", "found addr=0x%02X", static_cast<unsigned int>(addr));
    }
  }

  LOG_INFO("i2c", "scan_done found_count=%u", static_cast<unsigned int>(foundCount));
}

void testBeginSensors() {
  battery.begin();
  for (int i = 0; i < sensorCount; ++i) {
    Serial.print(sensors[i]->name());
    if (!sensors[i]->begin()) {
      Serial.print(" begin FAILED.");
    } else {

      Serial.print(" begin OK.");
    }
    delay(1000);

    if (!sensors[i]->wake()) {
      Serial.println(" wake FAILED");
    } else {

      Serial.println(" wake OK");
    }
  }
}

void testSampleSensors() {
  for (int i = 0; i < sensorCount; ++i) {
    sensors[i]->service();
    if (sensors[i]->ready()) {
      if (sensors[i]->sample()) {
        char buf[180];
        sensors[i]->writeTelemetry(buf, sizeof(buf));
        Serial.println(buf);
      } else {
        Serial.print(sensors[i]->name());
        Serial.println(" sample failed");
        scanI2C();
      }
    } else {
      Serial.print(sensors[i]->name());
      Serial.println(" not ready");
      scanI2C();
    }
  }

  battery.sample();
  char buf[180];
  battery.writeTelemetry(buf, sizeof(buf));
  Serial.println(buf);
  Serial.println("-------------------------");
}

void testServiceSensors() {
  for (int i = 0; i < sensorCount; ++i) {
    sensors[i]->service();
  }
}

void setup() {
  delay(5000);
  Serial.begin(115200);
  Serial1.begin(115200);
  while (!Serial1 && millis() < 3000) {
  }

  gLog.setMinLevel(LogLevel::Debug);

  LOG_INFO("boot", "SmartFires node starting");
  LOG_INFO("boot", "node_id=%u", initialRadioAddr);

  gps.reset();

  Wire.begin();
  delay(100);
  scanI2C();
  // duty.resetSensors();
  // testBeginSensors();
  LOG_INFO("boot", "SmartFires Feather TDMA node starting");

  LOG_INFO("boot", "uid_hash=0x%08lX", static_cast<unsigned long>(nodeUidHash));
  LOG_INFO("boot", "radio_addr_init=%u",
           static_cast<unsigned int>(initialRadioAddr));

  LOG_INFO("tdma", "entities=%u", static_cast<unsigned int>(numSlots));
  LOG_INFO("tdma", "slot_width_ms=%lu",
           static_cast<unsigned long>(tdmaCfg.slotWidthMs));
  LOG_INFO("tdma", "guard_ms=%lu", static_cast<unsigned long>(tdmaCfg.guardMs));
  LOG_INFO("tdma", "sync_stale_s=%lu",
           static_cast<unsigned long>(tdmaCfg.syncStaleMs / 1000UL));

  LOG_INFO("tdma", "app_reliability=%s",
           tdmaCfg.enableAppReliability ? "ON" : "OFF");
  LOG_INFO("tdma", "link_ack=%s",
           tdmaCfg.enableLinkAck ? "WAIT_FOR_ACK" : "FIRE_AND_FORGET");
  LOG_INFO("tdma", "queue_depth=%u",
           static_cast<unsigned int>(tdmaCfg.queueDepth));
  LOG_INFO("tdma", "retx_window=%u",
           static_cast<unsigned int>(tdmaCfg.reliabilityWindowDepth));
  LOG_INFO("tdma", "retx_max_attempts=%u",
           static_cast<unsigned int>(tdmaCfg.reliabilityMaxAttempts));
  LOG_INFO("tdma", "retx_max_age_ms=%lu",
           static_cast<unsigned long>(tdmaCfg.reliabilityMaxAgeMs));
  LOG_INFO("tdma", "link_retries=%u",
           static_cast<unsigned int>(tdmaCfg.maxRetries));
  LOG_INFO("tdma", "ack_timeout_ms=%lu",
           static_cast<unsigned long>(tdmaCfg.ackTimeoutMs));
  LOG_INFO("tdma", "telem_rel_mode=%s",
           reliabilityModeName(tdmaCfg.reliabilityMode));
  LOG_INFO("tdma", "ack_paced_gate: expected_ack_interval_ms=%lu mul_permille=%u wait_min_ms=%lu wait_max_ms=%lu req_ack_before_retx=%s",
           static_cast<unsigned long>(tdmaCfg.expectedAckIntervalMs),
           static_cast<unsigned int>(tdmaCfg.retryWaitMultiplierPermille),
           static_cast<unsigned long>(tdmaCfg.retryWaitMinMs),
           static_cast<unsigned long>(tdmaCfg.retryWaitMaxMs),
           tdmaCfg.requireAckSummaryBeforeFirstRetry ? "YES" : "NO");

  LOG_INFO("packet", "status_interval_ms=%lu",
           static_cast<unsigned long>(packetHandlerCfg.statusIntervalMs));
  LOG_INFO(
      "packet", "status_interval_s=%lu",
      static_cast<unsigned long>(packetHandlerCfg.statusIntervalMs / 1000UL));
  LOG_INFO(
      "packet", "status_interval_min=%lu",
      static_cast<unsigned long>(packetHandlerCfg.statusIntervalMs / 60000UL));

  if (!app.begin()) {
    LOG_INFO("boot", "smart_fires_app_status=%d", 1);
    while (true) {
      delay(500);
    }
  }

  LOG_INFO("boot", "smart_fires_app_status=%d", 0);
}

unsigned long previousMillis = 0; // Stores last time event triggered
const long interval = 500;        // Interval (milliseconds)

void loop() {
  // unsigned long currentMillis = millis();
  // testServiceSensors();
  // if (currentMillis - previousMillis >= interval) {
  //   previousMillis = currentMillis;
  //   testSampleSensors();
  // }
  app.update();
  delay(25);
}

#else
#error "Define exactly one firmware role: LORA_BASE or LORA_NODE"
#endif
