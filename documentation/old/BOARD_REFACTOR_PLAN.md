# Project Structure
SmartFires_IoT/
├── platformio.ini
│
├── include/
│   ├── app/
│   │   ├── SmartFiresNodeApp.h
│   │   ├── NodeConfig.h
│   │   └── NodeState.h
│   │
│   ├── interfaces/
│   │   ├── IClock.h
│   │   ├── ISensor.h
│   │   ├── IRadio.h
│   │   ├── IBatteryMonitor.h
│   │   ├── IAnalogReader.h
│   │   ├── IGpio.h
│   │   ├── ISerialPort.h
│   │   └── ILedStatus.h
│   │
│   ├── sensors/
│   │   ├── Sht31Sensor.h
│   │   ├── WindSensorRevC.h
│   │   ├── Pa1010dGpsSensor.h
│   │   ├── Sps30Sensor.h
│   │   └── Icm20948Sensor.h
│   │
│   ├── drivers/
│   │   ├── ISht31Driver.h
│   │   ├── IGpsDriver.h
│   │   ├── ISps30Driver.h
│   │   └── IIcm20948Driver.h
│   │
│   ├── power/
│   │   ├── DutyCycleController.h
│   │   ├── SensorScheduler.h
│   │   ├── BatteryMonitor.h
│   │   └── PowerPolicy.h
│   │
│   ├── radio/
│   │   ├── RadioService.h
│   │   ├── RadioPacket.h
│   │   ├── PacketCodec.h
│   │   └── CommandHandler.h
│   │
│   ├── telemetry/
│   │   ├── TelemetryBuilder.h
│   │   └── TelemetryFrame.h
│   │
│   ├── status/
│   │   ├── LedStatusController.h
│   │   └── ErrorCode.h
│   │
│   └── platform/
│       ├── BoardPins_FeatherM0.h
│       ├── ArduinoClock.h
│       ├── ArduinoAnalogReader.h
│       ├── ArduinoGpio.h
│       ├── ArduinoSerialPort.h
│       ├── AdafruitSht31Driver.h
│       ├── AdafruitGpsDriver.h
│       ├── SensirionSps30Driver.h
│       ├── SparkfunIcm20948Driver.h
│       ├── RadioHeadRfm95Radio.h
│       └── FeatherBatteryMonitor.h
│
├── src/
│   ├── main.cpp
│   ├── app/
│   ├── sensors/
│   ├── power/
│   ├── radio/
│   ├── telemetry/
│   ├── status/
│   └── platform/
│
└── test/
    ├── fakes/
    │   ├── FakeClock.h
    │   ├── FakeSensor.h
    │   ├── FakeRadio.h
    │   ├── FakeBatteryMonitor.h
    │   ├── FakeAnalogReader.h
    │   ├── FakeGpio.h
    │   ├── FakeSht31Driver.h
    │   ├── FakeGpsDriver.h
    │   ├── FakeSps30Driver.h
    │   └── FakeIcm20948Driver.h
    │
    ├── test_sht31_sensor/
    ├── test_wind_sensor/
    ├── test_gps_sensor/
    ├── test_sps30_sensor/
    ├── test_imu_sensor/
    ├── test_battery_monitor/
    ├── test_duty_cycle_controller/
    ├── test_radio_service/
    ├── test_telemetry_builder/
    └── test_node_app/

# Architecture

main.cpp
  creates real hardware drivers
  creates sensors
  creates radio
  creates battery monitor
  creates app

SmartFiresNodeApp
  owns high-level loop behavior only

DutyCycleController
  decides when sensors wake, sample, sleep

Sensor classes
  know how to begin, wake, sleep, service, sample

BatteryMonitor
  reads voltage / percent / low-battery state

RadioService
  sends telemetry
  receives commands
  handles ACK/retry if needed

TelemetryBuilder
  converts latest readings into compact packets

LedStatusController
  shows boot/error/radio/battery state

# Test categories 
SHT31 unit test:
- begin succeeds when fake driver succeeds
- begin fails when fake driver fails
- wake enters Waking
- service transitions Waking -> Ready after wakeDelayMs
- sample stores temp/humidity
- sample fails on NaN
- sleep enters Sleeping

GPS unit test:
- begin succeeds
- no fix produces valid=false
- fix produces lat/lon/time
- GPS can be AlwaysOn
- sample period is respected

SPS30 unit test:
- begin starts correctly
- wake starts measurement
- service waits for warmup
- sample fails if not measuring
- sleep stops measurement

Wind unit test:
- raw ADC converts to voltage
- divider ratio is applied
- sample period is respected
- invalid ADC values handled

Duty-cycle integration test:
- sleeps sensors
- wakes duty-cycled sensors
- leaves GPS always on
- waits for warmup sensors
- samples ready sensors
- builds telemetry
- sends radio packet
- goes back to sleep
