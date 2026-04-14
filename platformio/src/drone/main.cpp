#ifndef PIO_UNIT_TESTING

#include <Arduino.h>
#include <Wire.h>

#include "FlameSensor.h"
#include "Icm20948Imu.h"
#include "LidarLiteV3.h"
#include "MatrixKeypadSensor.h"
#include "OledDisplay.h"
#include "Pa1010dGpsSensor.h"
#include "PinMapping.h"
#include "Sht31Sensor.h"
#include "WindSensorRevC.h"
#include "shared/UartLoRaBridge.h"

#include "app/DroneApp.h"
#include "app/DroneContext.h"
#include "app/DroneState.h"
#include "app/SensorManager.h"
#include "app/ActuatorManager.h"
#include "app/TelemetryService.h"
#include "app/LinkService.h"
#include "app/KeypadController.h"
#include "app/OledPageController.h"
#include "platform/ArduinoClock.h"

#ifndef NODE_ID
#error "NODE_ID must be set in platformio.ini build_flags (e.g. -D NODE_ID=1)"
#endif

namespace {
WindSensorRevC wind(PIN_WIND_RV, PIN_WIND_TMP);
Icm20948Imu imu(1);
FlameSensor flame(PIN_FLAME_AO, PIN_FLAME_DO);
Sht31Sensor sht31(Sht31Sensor::kAlternateAddress);
Pa1010dGpsSensor gps(Wire);
LidarLiteV3 lidar;
MatrixKeypadSensor keypad(KEYPAD_ROWS, KEYPAD_COLS, KEYPAD_MAP, "Drone Keypad");

OledDisplay oled(OledDisplay::Controller::SH1106, 0x3C, "OLED Display");

HardwareSerial LoRaSerial(1);
UartLoRaBridge bridge(LoRaSerial, PIN_LORA_RX, PIN_LORA_TX, 115200);

ISensor* sensors[] = {&flame, &imu, &sht31, &gps, &wind, &lidar, &keypad};
IActuator* actuators[] = {&oled};

DroneContext ctx{
  wind, imu, flame, sht31, gps, lidar, keypad, oled, bridge,
  sensors, sizeof(sensors) / sizeof(sensors[0]),
  actuators, sizeof(actuators) / sizeof(actuators[0])
};

AppState state{};
ArduinoClock clock;
SensorManager sensorManager(ctx);
ActuatorManager actuatorManager(ctx);
TelemetryService telemetry(ctx);
LinkService link(ctx, state, clock, telemetry);
KeypadController keypadController(ctx, state);
OledPageController oledController(ctx, state, clock);
DroneApp app(NODE_ID, ctx, state, clock, sensorManager, actuatorManager,
             telemetry, link, keypadController, oledController);
}

void setup() {
  app.setup();
}

void loop() {
  app.loop();
}
#endif
