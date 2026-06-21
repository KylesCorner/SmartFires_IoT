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

#include "config/NetworkConfig.h"
#include "config/SensingConfig.h"
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

namespace {

constexpr uint8_t kUnassignedNodeId = 0x00;

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
  uint8_t addr = static_cast<uint8_t>(uidHash & 0xFFu);
  if (addr == 0x00u) addr = 0x01u;
  if (addr == 0xFFu) addr = 0xFEu;
  return addr;
}

// Boot-time validation: warns if a sensor's own minSamplePeriodMs floor
// (config/SensingConfig.h) is slower than the duty-cycle cadence actually
// driving it (config/SensingConfig.h's DutyCycle::kContinuousSamplePeriodMs).
// This is not necessarily a bug — a sensor slower than the master loop
// simply won't produce a fresh sample on every cycle, by design — but it
// was previously impossible to even check, since the two values lived in
// unrelated files with no comparison between them.
void logSensorFloorVsCadence(const char *sensorName, uint32_t minSamplePeriodMs) {
  if (minSamplePeriodMs > SensingConfig::DutyCycle::kContinuousSamplePeriodMs) {
    LOG_WARN("sensing",
             "sensor_floor_above_cadence sensor=%s min_period_ms=%lu "
             "cadence_ms=%lu (expected if intentional; sensor will not "
             "produce a fresh sample on every cycle)",
             sensorName, static_cast<unsigned long>(minSamplePeriodMs),
             static_cast<unsigned long>(
                 SensingConfig::DutyCycle::kContinuousSamplePeriodMs));
  }
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
TPSDriver::Config windPowerCfg = TPSDriver::Config::make(PIN_WIND_ENABLE, true);
TPSDriver windPower(windPowerCfg);
WindSensorRevC::Config windCfg =
    WindSensorRevC::Config::make(PIN_WIND_RV,
                                PIN_WIND_TMP,
                                SensingConfig::Wind::kMinSamplePeriodMs,
                                SensingConfig::Wind::kWakeDelayMs,
                                SensingConfig::Wind::kDutyClass);
WindSensorRevC wind(windCfg, analog, windPower, clock);

AdafruitSht31Driver sht31Driver;
Sht31Sensor::Config sht31Cfg = Sht31Sensor::Config::make(
  SensingConfig::Sht31::kMinSamplePeriodMs,
  SensingConfig::Sht31::kDutyClass);
Sht31Sensor sht31(sht31Cfg, sht31Driver, clock);

AdafruitGpsDriver gpsDriver;
Pa1010dGpsSensor::Config gpsCfg = Pa1010dGpsSensor::Config::make(
    SensingConfig::Gps::kPeriodicRunTimeMs,
    SensingConfig::Gps::kPeriodicSleepTimeMs,
    SensingConfig::Gps::kPeriodicSecondRunTimeMs,
    SensingConfig::Gps::kPeriodicSecondSleepTimeMs,
    SensingConfig::Gps::kPeriodicMinSamplePeriodMs,
    GpsPowerMode::PeriodicBackup);
Pa1010dGpsSensor gps(gpsCfg, gpsDriver, clock);

SparkfunIcm20948Driver imuDriver;
Icm20948Sensor::Config imuCfg = Icm20948Sensor::Config::make(
  SensingConfig::Imu::kMinSamplePeriodMs,
  SensingConfig::Imu::kWakeDelayMs,
  SensingConfig::Imu::kDutyClass
);
Icm20948Sensor imu(imuCfg, imuDriver, clock);

Sps30Sensor::Config sps30Cfg = Sps30Sensor::Config::make(
    SensingConfig::Sps30::kMinSamplePeriodMs,
    SensingConfig::Sps30::kWakeDelayMs,
    SensingConfig::Sps30::kDutyClass);

SensirionUartSps30Driver sps30Driver(Serial1);
Sps30Sensor sps30(sps30Cfg, sps30Driver, clock);

ISensor *sensors[] = {
    &sht31, &gps, &imu, &sps30, &wind,
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

DutyCycleConfig dutyCfg = DutyCycleConfig::make(
    SensingConfig::DutyCycle::kThresholdEnabled,
    SensingConfig::DutyCycle::kThresholdMinSleepMs,
    SensingConfig::DutyCycle::kThresholdMaxWakeMs,
    SensingConfig::DutyCycle::kThresholdActiveSampleMs,
    SensingConfig::DutyCycle::kThresholdSamplePeriodMs,
    SensingConfig::DutyCycle::kThresholdWarmupMs,
    SensingConfig::DutyCycle::kThresholdTempDeltaThresholdC,
    SensingConfig::DutyCycle::kThresholdHumidityDeltaThresholdPct,
    SensingConfig::DutyCycle::kThresholdFailOnSampleError);
DutyCycleController duty(dutyCfg, sht31, sensors, sensorCount, clock, battery);

// -----------------------------------------------------------------------------
// Networking
// -----------------------------------------------------------------------------

constexpr uint8_t numSlots = NetworkConfig::kNumSlots;
const uint32_t nodeUidHash = BoardIdentity::hash32();
const uint8_t initialRadioAddr = makeInitialRadioAddr(nodeUidHash);

PacketHandler::Config packetHandlerCfg = PacketHandler::Config::make(
    kUnassignedNodeId, BinaryPacket::kBundleMaxDeltas,
    NetworkConfig::kStatusIntervalMs);
PacketHandler packetHandler(packetHandlerCfg);

// Single named profile from config/NetworkConfig.h — replaces the old
// makeNodeTdmaCfg() helper, which built a TdmaConfig from TdmaConfig's own
// (unused-in-practice) defaults and then re-assigned 11 fields by hand.
TdmaConfig tdmaCfg = NetworkConfig::nodeTdmaProfile();
TdmaClock tdmaClock(tdmaCfg, clock);
TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);

// radioHeadCfg() now sources every field besides `address` from
// NetworkConfig.h directly, so there is no separate makeNodeRadioCfg()
// wrapper threading tdmaCfg.ackTimeoutMs into it by hand anymore — both
// already come from the same NetworkConfig::kLinkAckTimeoutMs constant.
RadioHeadTdmaDriver::Config radioDriverCfg =
    RadioHeadTdmaDriver::Config::radioHeadCfg(initialRadioAddr);
RadioHeadTdmaDriver radioDriver(radioDriverCfg);

TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

SmartFiresNodeApp::Config appCfg = SmartFiresNodeApp::Config::appCfg(
  kUnassignedNodeId, nodeUidHash, true, false,
  NetworkConfig::kEnableTelemetryTx);

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
  analog.begin();
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

  LOG_INFO("sensing", "cadence_ms=%lu",
           static_cast<unsigned long>(
               SensingConfig::DutyCycle::kContinuousSamplePeriodMs));
  logSensorFloorVsCadence("sht31", sht31Cfg.minSamplePeriodMs);
  logSensorFloorVsCadence("wind", windCfg.minSamplePeriodMs);
  logSensorFloorVsCadence("gps", gpsCfg.minSamplePeriodMs);
  logSensorFloorVsCadence("imu", imuCfg.minSamplePeriodMs);
  logSensorFloorVsCadence("sps30", sps30Cfg.minSamplePeriodMs);

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
