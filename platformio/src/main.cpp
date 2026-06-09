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

#include "platform/AdafruitGpsDriver.h"
#include "platform/AdafruitSht31Driver.h"
// #include "platform/SensirionUartSps30Driver.h"
#include "platform/SparkfunIcm20948Driver.h"
#include "platform/TPSDriver.h"
#include "platform/SparkfunBmv080Driver.h"

#include "sensors/Icm20948Sensor.h"
#include "sensors/Pa1010dGpsSensor.h"
#include "sensors/Sht31Sensor.h"
// #include "sensors/Sps30Sensor.h"
#include "sensors/WindSensorRevC.h"
#include "sensors/Bmv080Sensor.h"


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

// Sps30Sensor::Config sps30Cfg = Sps30Sensor::Config::makeSps30Cfg();
// SensirionUartSps30Driver sps30Driver(Serial1);
// Sps30Sensor sps30(sps30Cfg, sps30Driver, clock);

Bmv080Sensor::Config bmv080Cfg =
    Bmv080Sensor::Config::makeBmv080Cfg(
        0x57,                       // Bosch BMV080 shuttle board address
        1000,                       // min sample period ms
        1000,                       // wake delay ms
        SensorDutyClass::AlwaysOn);
SparkfunBmv080Driver bmv080Driver(Wire);
Bmv080Sensor bmv080(bmv080Cfg, bmv080Driver, clock);

ISensor *sensors[] = {
    &sht31, &gps, &imu, &bmv080, &wind,
};

// ISensor *sensors[] = {
//     &sht31,
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
    kUnassignedNodeId, nodeUidHash, true, false);

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
  LOG_INFO("test_begin", "begin_sensors_start count=%u",
           static_cast<unsigned int>(sensorCount));

  battery.begin();

  for (size_t i = 0; i < sensorCount; ++i) {
    ISensor *sensor = sensors[i];

    LOG_INFO("test_begin", "sensor_begin_start index=%u name=%s",
             static_cast<unsigned int>(i), sensor->name());

    const bool beginOk = sensor->begin();

    LOG_INFO("test_begin", "sensor_begin_done index=%u name=%s ok=%u healthy=%u state=%u",
             static_cast<unsigned int>(i),
             sensor->name(),
             beginOk ? 1 : 0,
             sensor->healthy() ? 1 : 0,
             static_cast<unsigned int>(sensor->powerState()));

    delay(1000);

    const bool wakeOk = sensor->wake();

    LOG_INFO("test", "sensor_wake_done index=%u name=%s ok=%u ready=%u healthy=%u state=%u",
             static_cast<unsigned int>(i),
             sensor->name(),
             wakeOk ? 1 : 0,
             sensor->ready() ? 1 : 0,
             sensor->healthy() ? 1 : 0,
             static_cast<unsigned int>(sensor->powerState()));
  }

  LOG_INFO("test", "begin_sensors_done");
}
void testServiceSensors() {
  for (size_t i = 0; i < sensorCount; ++i) {
    ISensor *sensor = sensors[i];

    const bool serviceOk = sensor->service();
    const bool ready = sensor->ready();

    // LOG_DEBUG("test",
    //           "sensor_service index=%u name=%s service_ok=%u ready=%u healthy=%u state=%u",
    //           static_cast<unsigned int>(i),
    //           sensor->name(),
    //           serviceOk ? 1 : 0,
    //           ready ? 1 : 0,
    //           sensor->healthy() ? 1 : 0,
    //           static_cast<unsigned int>(sensor->powerState()));
  }
}
void testSampleSensors() {
  // LOG_DEBUG("test", "sample_sensors_start count=%u",
  //           static_cast<unsigned int>(sensorCount));

  for (size_t i = 0; i < sensorCount; ++i) {
    ISensor *sensor = sensors[i];

    // LOG_DEBUG("test", "sensor_loop_start index=%u name=%s",
    //           static_cast<unsigned int>(i), sensor->name());

    // LOG_DEBUG("test", "sensor_service_start index=%u name=%s",
    //           static_cast<unsigned int>(i), sensor->name());

    const bool serviceOk = sensor->service();

    LOG_DEBUG("test_sample",
              "sensor_service_done index=%u name=%s service_ok=%u ready=%u healthy=%u state=%u",
              static_cast<unsigned int>(i),
              sensor->name(),
              serviceOk ? 1 : 0,
              sensor->ready() ? 1 : 0,
              sensor->healthy() ? 1 : 0,
              static_cast<unsigned int>(sensor->powerState()));

    if (!sensor->healthy()) {
      LOG_WARN("test_sample", "sensor_sample_skip index=%u name=%s reason=not_healthy state=%u",
               static_cast<unsigned int>(i),
               sensor->name(),
               static_cast<unsigned int>(sensor->powerState()));
      continue;
    }

    if (!sensor->ready()) {
      LOG_DEBUG("test_sample", "sensor_sample_skip index=%u name=%s reason=not_ready state=%u",
                static_cast<unsigned int>(i),
                sensor->name(),
                static_cast<unsigned int>(sensor->powerState()));
      continue;
    }

    // LOG_INFO("test", "sensor_sample_start index=%u name=%s",
    //          static_cast<unsigned int>(i), sensor->name());

    const bool sampleOk = sensor->sample();

    // LOG_INFO("test", "sensor_sample_done index=%u name=%s ok=%u",
    //          static_cast<unsigned int>(i),
    //          sensor->name(),
    //          sampleOk ? 1 : 0);

    // char buf[180];
    // sensor->writeTelemetry(buf, sizeof(buf));

    // LOG_INFO("test", "sensor_sample_ok index=%u name=%s telemetry=%s",
    //          static_cast<unsigned int>(i),
    //          sensor->name(),
    //          buf);
  //   char buf[180];

  //   LOG_INFO("test", "sensor_write_telemetry_start index=%u name=%s",
  //           static_cast<unsigned int>(i),
  //           sensor->name());

  //   const size_t written = sensor->writeTelemetry(buf, sizeof(buf));

  //   LOG_INFO("test", "sensor_write_telemetry_done index=%u name=%s written=%u",
  //           static_cast<unsigned int>(i),
  //           sensor->name(),
  //           static_cast<unsigned int>(written));

  //   LOG_INFO("test", "sensor_sample_ok index=%u name=%s telemetry=%s",
  //           static_cast<unsigned int>(i),
  //           sensor->name(),
  //           buf);
  // }

  // LOG_DEBUG("test", "battery_sample_start");

  // if (battery.sample()) {
  //   char buf[180];
  //   battery.writeTelemetry(buf, sizeof(buf));
  //   LOG_INFO("test", "battery_sample_ok telemetry=%s", buf);
  // } else {
  //   LOG_WARN("test", "battery_sample_failed");
  // }

  // LOG_DEBUG("test", "sample_sensors_done");
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

  // gps.reset();

  Wire.begin();
  delay(100);
  scanI2C();
  // duty.resetSensors();
  testBeginSensors();
  // LOG_INFO("boot", "SmartFires Feather TDMA node starting");

  // LOG_INFO("boot", "uid_hash=0x%08lX", static_cast<unsigned long>(nodeUidHash));
  // LOG_INFO("boot", "radio_addr_init=%u",
  //          static_cast<unsigned int>(initialRadioAddr));

  // LOG_INFO("tdma", "entities=%u", static_cast<unsigned int>(numSlots));
  // LOG_INFO("tdma", "slot_width_ms=%lu",
  //          static_cast<unsigned long>(tdmaCfg.slotWidthMs));
  // LOG_INFO("tdma", "guard_ms=%lu", static_cast<unsigned long>(tdmaCfg.guardMs));
  // LOG_INFO("tdma", "sync_stale_s=%lu",
  //          static_cast<unsigned long>(tdmaCfg.syncStaleMs / 1000UL));

  // LOG_INFO("tdma", "app_reliability=%s",
  //          tdmaCfg.enableAppReliability ? "ON" : "OFF");
  // LOG_INFO("tdma", "link_ack=%s",
  //          tdmaCfg.enableLinkAck ? "WAIT_FOR_ACK" : "FIRE_AND_FORGET");
  // LOG_INFO("tdma", "retx_window=%u",
  //          static_cast<unsigned int>(tdmaCfg.reliabilityWindowDepth));
  // LOG_INFO("tdma", "retx_max_attempts=%u",
  //          static_cast<unsigned int>(tdmaCfg.reliabilityMaxAttempts));
  // LOG_INFO("tdma", "link_retries=%u",
  //          static_cast<unsigned int>(tdmaCfg.maxRetries));
  // LOG_INFO("tdma", "ack_timeout_ms=%lu",
  //          static_cast<unsigned long>(tdmaCfg.ackTimeoutMs));
  // LOG_INFO("tdma", "telem_rel_mode=%s",
  //          reliabilityModeName(tdmaCfg.reliabilityMode));

  // LOG_INFO("packet", "status_interval_ms=%lu",
  //          static_cast<unsigned long>(packetHandlerCfg.statusIntervalMs));
  // LOG_INFO(
  //     "packet", "status_interval_s=%lu",
  //     static_cast<unsigned long>(packetHandlerCfg.statusIntervalMs / 1000UL));
  // LOG_INFO(
  //     "packet", "status_interval_min=%lu",
  //     static_cast<unsigned long>(packetHandlerCfg.statusIntervalMs / 60000UL));

  // if (!app.begin()) {
  //   LOG_INFO("boot", "smart_fires_app_status=%d", 1);
  //   while (true) {
  //     delay(500);
  //   }
  // }

  // LOG_INFO("boot", "smart_fires_app_status=%d", 0);
}

unsigned long previousMillis = 0; // Stores last time event triggered
const long interval = 1000;        // Interval (milliseconds)

void loop() {
  unsigned long currentMillis = millis();
  testServiceSensors();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    testSampleSensors();
    Serial.println("sampled");
  }
  // app.update();
  delay(25);
}

#else
#error "Define exactly one firmware role: LORA_BASE or LORA_NODE"
#endif
