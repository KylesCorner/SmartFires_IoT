// ---
// description: Firmware entrypoint — selects and wires up one of LORA_BASE/LORA_NODE/SENSOR_PROBE build roles, constructing all objects and running setup()/loop().
// role: entrypoint
// ---
#include "logging/DebugLogger.h"
#include <Arduino.h>

#if defined(LORA_BASE)

#include "app/SmartFiresBaseApp.h"
#include "logging/FramedDebugLogSink.h"
#include "platform/ArduinoClock.h"
#include "platform/RadioHeadTdmaDriver.h"

ArduinoClock baseClock;

RadioHeadTdmaDriver::Config baseRadioCfg =
    RadioHeadTdmaDriver::Config::radioHeadCfg(0x01);
RadioHeadTdmaDriver baseRadio(baseRadioCfg);

SmartFiresBaseApp::Config baseAppCfg = SmartFiresBaseApp::Config::baseCfg();
// Jetson link is native USB CDC (Serial), matching the sniffer firmware — see
// documentation/Current_Architecture/UART_JETSON_BRIDGE.md. Debug logs are
// multiplexed onto that same link as PKT_DEBUG_LOG frames (FramedDebugLogSink)
// instead of a separate physical UART — Serial1 is unused on this build.
FramedDebugLogSink baseDebugSink(Serial, baseAppCfg.baseAddr);
SmartFiresBaseApp baseApp(baseAppCfg, baseClock, baseRadio, Serial, baseDebugSink);
DebugLogger gLog(baseDebugSink, baseAppCfg.baseAddr);

void setup() {
  Serial.begin(baseAppCfg.uartBaud);
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

// -----------------------------------------------------------------------------
// Methods
// -----------------------------------------------------------------------------

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
  app.update();
  delay(25);
}

#elif defined(POWER_TEST)
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include "logging/DebugLogger.h"

#include "config/SensingConfig.h"
#include "interfaces/ISensor.h"

#include "platform/ArduinoAnalogReader.h"
#include "platform/ArduinoClock.h"

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

// -----------------------------------------------------------------------------
// Power-test modes
// -----------------------------------------------------------------------------

#define POWER_TEST_MODE_MCU_RUN       1
#define POWER_TEST_MODE_MCU_STANDBY   2
#define POWER_TEST_MODE_I2C_IDLE      3
#define POWER_TEST_MODE_RADIO_STANDBY 4
#define POWER_TEST_MODE_RADIO_RX      5

#define POWER_TEST_MODE_SHT31         10
#define POWER_TEST_MODE_IMU           11
#define POWER_TEST_MODE_GPS           12
#define POWER_TEST_MODE_SPS30         13
#define POWER_TEST_MODE_WIND          14

#ifndef POWER_TEST_MODE
#define POWER_TEST_MODE POWER_TEST_MODE_MCU_RUN
#endif

#ifndef POWER_TEST_USE_SERIAL
#define POWER_TEST_USE_SERIAL 1
#endif

#ifndef POWER_TEST_SAMPLE_PERIOD_MS
#define POWER_TEST_SAMPLE_PERIOD_MS 1000
#endif

#ifndef POWER_TEST_PRE_SLEEP_DELAY_MS
#define POWER_TEST_PRE_SLEEP_DELAY_MS 5000
#endif

#if POWER_TEST_MODE == POWER_TEST_MODE_SHT31 || \
    POWER_TEST_MODE == POWER_TEST_MODE_IMU ||   \
    POWER_TEST_MODE == POWER_TEST_MODE_GPS ||   \
    POWER_TEST_MODE == POWER_TEST_MODE_I2C_IDLE
#define POWER_TEST_NEEDS_I2C 1
#else
#define POWER_TEST_NEEDS_I2C 0
#endif

#if POWER_TEST_MODE == POWER_TEST_MODE_WIND
#define POWER_TEST_NEEDS_ANALOG 1
#else
#define POWER_TEST_NEEDS_ANALOG 0
#endif

#if POWER_TEST_MODE == POWER_TEST_MODE_SPS30
#define POWER_TEST_NEEDS_SERIAL1 1
#else
#define POWER_TEST_NEEDS_SERIAL1 0
#endif

#if POWER_TEST_MODE == POWER_TEST_MODE_SHT31 || \
    POWER_TEST_MODE == POWER_TEST_MODE_IMU ||   \
    POWER_TEST_MODE == POWER_TEST_MODE_GPS ||   \
    POWER_TEST_MODE == POWER_TEST_MODE_SPS30 || \
    POWER_TEST_MODE == POWER_TEST_MODE_WIND
#define POWER_TEST_HAS_SENSOR 1
#else
#define POWER_TEST_HAS_SENSOR 0
#endif

// -----------------------------------------------------------------------------
// Feather M0 LoRa / RFM95 pins
// -----------------------------------------------------------------------------

constexpr uint8_t PIN_RFM95_CS  = 8;
constexpr uint8_t PIN_RFM95_RST = 4;
constexpr uint8_t PIN_RFM95_IRQ = 3;

// -----------------------------------------------------------------------------
// SX127x / RFM95 registers
// -----------------------------------------------------------------------------

constexpr uint8_t RFM95_REG_OP_MODE = 0x01;
constexpr uint8_t RFM95_REG_VERSION = 0x42;

constexpr uint8_t RFM95_WRITE_MASK = 0x80;
constexpr uint8_t RFM95_READ_MASK  = 0x7F;

constexpr uint8_t RFM95_LONG_RANGE_MODE = 0x80;
constexpr uint8_t RFM95_MODE_SLEEP      = 0x00;
constexpr uint8_t RFM95_MODE_STDBY      = 0x01;
constexpr uint8_t RFM95_MODE_RX_CONT    = 0x05;

SPISettings rfm95SpiSettings(1000000, MSBFIRST, SPI_MODE0);

// -----------------------------------------------------------------------------
// Platform
// -----------------------------------------------------------------------------

ArduinoClock clock;
ArduinoAnalogReader analog;
DebugLogger gLog(Serial, 0xEE);
// -----------------------------------------------------------------------------
// Optional sensor objects
// -----------------------------------------------------------------------------

#if POWER_TEST_MODE == POWER_TEST_MODE_WIND

constexpr uint8_t PIN_WIND_RV = A1;
constexpr uint8_t PIN_WIND_TMP = A2;
constexpr uint8_t PIN_WIND_ENABLE = A3;

TPSDriver::Config windPowerCfg =
    TPSDriver::Config::make(PIN_WIND_ENABLE, true);

TPSDriver windPower(windPowerCfg);

WindSensorRevC::Config windCfg = WindSensorRevC::Config::make(
    PIN_WIND_RV,
    PIN_WIND_TMP,
    SensingConfig::Wind::kMinSamplePeriodMs,
    SensingConfig::Wind::kWakeDelayMs,
    SensingConfig::Wind::kDutyClass);

WindSensorRevC activeSensor(windCfg, analog, windPower, clock);

#elif POWER_TEST_MODE == POWER_TEST_MODE_SHT31

AdafruitSht31Driver sht31Driver;

Sht31Sensor::Config sht31Cfg = Sht31Sensor::Config::make(
    SensingConfig::Sht31::kMinSamplePeriodMs,
    SensingConfig::Sht31::kDutyClass);

Sht31Sensor activeSensor(sht31Cfg, sht31Driver, clock);

#elif POWER_TEST_MODE == POWER_TEST_MODE_GPS

AdafruitGpsDriver gpsDriver;

Pa1010dGpsSensor::Config gpsCfg = Pa1010dGpsSensor::Config::make(
    SensingConfig::Gps::kPeriodicRunTimeMs,
    SensingConfig::Gps::kPeriodicSleepTimeMs,
    SensingConfig::Gps::kPeriodicSecondRunTimeMs,
    SensingConfig::Gps::kPeriodicSecondSleepTimeMs,
    SensingConfig::Gps::kPeriodicMinSamplePeriodMs,
    GpsPowerMode::FullPowerContinuous);

Pa1010dGpsSensor activeSensor(gpsCfg, gpsDriver, clock);

#elif POWER_TEST_MODE == POWER_TEST_MODE_IMU

SparkfunIcm20948Driver imuDriver;

Icm20948Sensor::Config imuCfg = Icm20948Sensor::Config::make(
    SensingConfig::Imu::kMinSamplePeriodMs,
    SensingConfig::Imu::kWakeDelayMs,
    SensingConfig::Imu::kDutyClass);

Icm20948Sensor activeSensor(imuCfg, imuDriver, clock);

#elif POWER_TEST_MODE == POWER_TEST_MODE_SPS30

Sps30Sensor::Config sps30Cfg = Sps30Sensor::Config::make(
    SensingConfig::Sps30::kMinSamplePeriodMs,
    SensingConfig::Sps30::kWakeDelayMs,
    SensingConfig::Sps30::kDutyClass);

SensirionUartSps30Driver sps30Driver(Serial1);
Sps30Sensor activeSensor(sps30Cfg, sps30Driver, clock);

#endif

// -----------------------------------------------------------------------------
// Logging helpers
// -----------------------------------------------------------------------------

void logLine(const char *msg) {
#if POWER_TEST_USE_SERIAL
  Serial.println(msg);
#else
  (void)msg;
#endif
}

void logHex8(const char *label, uint8_t value) {
#if POWER_TEST_USE_SERIAL
  Serial.print(label);
  Serial.print("0x");

  if (value < 0x10) {
    Serial.print('0');
  }

  Serial.println(value, HEX);
#else
  (void)label;
  (void)value;
#endif
}

const char *powerTestModeName() {
  switch (POWER_TEST_MODE) {
  case POWER_TEST_MODE_MCU_RUN:
    return "MCU_RUN_RADIO_SLEEP";
  case POWER_TEST_MODE_MCU_STANDBY:
    return "MCU_STANDBY_RADIO_SLEEP";
  case POWER_TEST_MODE_I2C_IDLE:
    return "I2C_IDLE";
  case POWER_TEST_MODE_RADIO_STANDBY:
    return "RADIO_STANDBY";
  case POWER_TEST_MODE_RADIO_RX:
    return "RADIO_RX_CONTINUOUS";
  case POWER_TEST_MODE_SHT31:
    return "SHT31_ONLY";
  case POWER_TEST_MODE_IMU:
    return "IMU_ONLY";
  case POWER_TEST_MODE_GPS:
    return "GPS_ONLY";
  case POWER_TEST_MODE_SPS30:
    return "SPS30_ONLY";
  case POWER_TEST_MODE_WIND:
    return "WIND_ONLY";
  default:
    return "UNKNOWN";
  }
}

// -----------------------------------------------------------------------------
// RFM95 raw SPI helpers
// -----------------------------------------------------------------------------

void rfm95Select() {
  digitalWrite(PIN_RFM95_CS, LOW);
}

void rfm95Deselect() {
  digitalWrite(PIN_RFM95_CS, HIGH);
}

void rfm95WriteReg(uint8_t reg, uint8_t value) {
  SPI.beginTransaction(rfm95SpiSettings);
  rfm95Select();
  SPI.transfer(reg | RFM95_WRITE_MASK);
  SPI.transfer(value);
  rfm95Deselect();
  SPI.endTransaction();
}

uint8_t rfm95ReadReg(uint8_t reg) {
  SPI.beginTransaction(rfm95SpiSettings);
  rfm95Select();
  SPI.transfer(reg & RFM95_READ_MASK);
  const uint8_t value = SPI.transfer(0x00);
  rfm95Deselect();
  SPI.endTransaction();

  return value;
}

void resetRfm95() {
  pinMode(PIN_RFM95_CS, OUTPUT);
  rfm95Deselect();

  pinMode(PIN_RFM95_RST, OUTPUT);
  pinMode(PIN_RFM95_IRQ, INPUT);

  digitalWrite(PIN_RFM95_RST, LOW);
  delay(2);
  digitalWrite(PIN_RFM95_RST, HIGH);
  delay(10);
}

void setRfm95Mode(uint8_t mode) {
  rfm95WriteReg(RFM95_REG_OP_MODE, RFM95_LONG_RANGE_MODE | mode);
  delay(2);

  const uint8_t opMode = rfm95ReadReg(RFM95_REG_OP_MODE);
  logHex8("radio: opmode=", opMode);
}

void configureRadioForPowerTest() {
  logLine("radio: reset_start");

  resetRfm95();

  logLine("radio: reset_done");

  SPI.begin();

  const uint8_t version = rfm95ReadReg(RFM95_REG_VERSION);
  logHex8("radio: version=", version);

#if POWER_TEST_MODE == POWER_TEST_MODE_RADIO_STANDBY
  logLine("radio: mode=standby");
  setRfm95Mode(RFM95_MODE_STDBY);
#elif POWER_TEST_MODE == POWER_TEST_MODE_RADIO_RX
  logLine("radio: mode=rx_continuous");
  setRfm95Mode(RFM95_MODE_RX_CONT);
#else
  logLine("radio: mode=sleep");
  setRfm95Mode(RFM95_MODE_SLEEP);
#endif

  rfm95Deselect();
  logLine("radio: config_done");
}

// -----------------------------------------------------------------------------
// Low-power helpers
// -----------------------------------------------------------------------------

void preparePinsForPowerTest() {
  pinMode(PIN_RFM95_CS, OUTPUT);
  digitalWrite(PIN_RFM95_CS, HIGH);

  pinMode(PIN_RFM95_RST, OUTPUT);
  digitalWrite(PIN_RFM95_RST, HIGH);

  pinMode(PIN_RFM95_IRQ, INPUT);

#if defined(LED_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
#endif
}

void detachUsbForSleep() {
#if defined(ARDUINO_ARCH_SAMD)
  USBDevice.detach();
#endif
}

void disableAdcForSleep() {
#if defined(ARDUINO_ARCH_SAMD)
  ADC->CTRLA.bit.ENABLE = 0;
  while (ADC->STATUS.bit.SYNCBUSY) {
  }
#endif
}

void enterStandbyForever() {
  noInterrupts();

  SPI.end();
  disableAdcForSleep();

  SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;

  interrupts();

  __DSB();
  __WFI();

  while (true) {
    __WFI();
  }
}

// -----------------------------------------------------------------------------
// Optional I2C scan
// -----------------------------------------------------------------------------

void scanI2C() {
#if POWER_TEST_NEEDS_I2C
  logLine("i2c: scan_start");

  uint8_t foundCount = 0;

  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    const uint8_t err = Wire.endTransmission();

    if (err == 0) {
      ++foundCount;

#if POWER_TEST_USE_SERIAL
      Serial.print("i2c: found addr=0x");
      if (addr < 0x10) {
        Serial.print('0');
      }
      Serial.println(addr, HEX);
#endif
    }
  }

#if POWER_TEST_USE_SERIAL
  Serial.print("i2c: scan_done found_count=");
  Serial.println(foundCount);
#endif
#endif
}

// -----------------------------------------------------------------------------
// Sensor helpers
// -----------------------------------------------------------------------------

void beginActiveSensor() {
#if POWER_TEST_HAS_SENSOR
  logLine("sensor: begin_start");

  if (!activeSensor.begin()) {
    logLine("sensor: begin_failed");
    return;
  }

  logLine("sensor: begin_ok");

  if (!activeSensor.wake()) {
    logLine("sensor: wake_failed");
  } else {
    logLine("sensor: wake_ok");
  }
#else
  logLine("sensor: none");
#endif
}

void serviceAndSampleActiveSensor() {
#if POWER_TEST_HAS_SENSOR
  activeSensor.service();

  if (!activeSensor.ready()) {
    return;
  }

  if (!activeSensor.sample()) {
    logLine("sensor: sample_failed");
    return;
  }

#if POWER_TEST_USE_SERIAL
  char buf[180];
  activeSensor.writeTelemetry(buf, sizeof(buf));
  Serial.print("sensor: ");
  Serial.println(buf);
#endif
#endif
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------

void setup() {
  preparePinsForPowerTest();

  delay(3000);

#if POWER_TEST_USE_SERIAL
  Serial.begin(115200);

  const uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart < 3000)) {
  }
  DebugLogger gLog(Serial, 0xEE);

  Serial.println();
  Serial.println("========================================");
  Serial.println("SmartFires power test");
  Serial.print("mode: ");
  Serial.println(powerTestModeName());
  Serial.println("app: disabled");
  Serial.println("tdma: disabled");
  Serial.println("packet layer: disabled");
  Serial.println("========================================");
#endif

#if POWER_TEST_NEEDS_SERIAL1
  Serial1.begin(115200);
  logLine("serial1: begin_115200");
#endif

#if POWER_TEST_NEEDS_I2C
  Wire.begin();
  logLine("i2c: begin");
  delay(100);
  scanI2C();
#endif

#if POWER_TEST_NEEDS_ANALOG
  analog.begin();
  logLine("analog: begin");
#endif

  configureRadioForPowerTest();

#if POWER_TEST_MODE == POWER_TEST_MODE_MCU_STANDBY
  logLine("sleep: entering standby soon");
  logLine("sleep: USB serial will disconnect");
#if POWER_TEST_USE_SERIAL
  Serial.flush();
#endif

  delay(POWER_TEST_PRE_SLEEP_DELAY_MS);

  detachUsbForSleep();
  delay(100);

  enterStandbyForever();
#endif

  beginActiveSensor();

  logLine("boot: ready");
}

void loop() {
  static uint32_t lastSampleMs = 0;
  static uint32_t lastTickMs = 0;

  const uint32_t now = millis();

#if POWER_TEST_HAS_SENSOR
  serviceAndSampleActiveSensor();
#endif

  if (now - lastSampleMs >= POWER_TEST_SAMPLE_PERIOD_MS) {
    lastSampleMs = now;

#if POWER_TEST_HAS_SENSOR
    serviceAndSampleActiveSensor();
#endif
  }

#if POWER_TEST_USE_SERIAL
  if (now - lastTickMs >= 1000) {
    lastTickMs = now;
    Serial.print("tick: mode=");
    Serial.print(powerTestModeName());
    Serial.print(" ms=");
    Serial.println(now);
  }
#endif

  delay(100);
}
#else
#error "Define exactly one firmware role: LORA_BASE, LORA_NODE or POWER_TEST"
#endif
