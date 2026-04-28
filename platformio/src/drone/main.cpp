#include "DutyCycleController.h"
#include "esp32-hal.h"
#ifndef PIO_UNIT_TESTING

#include <Arduino.h>
#include <Wire.h>

#include "Icm20948Imu.h"
#include "MatrixKeypadSensor.h"
#include "OledDisplay.h"
#include "Pa1010dGpsSensor.h"
#include "PinMapping.h"
#include "Sht31Sensor.h"
#include "Sps30UartSensor.h"
#include "WindSensorRevC.h"
#include "shared/UartLoRaBridge.h"

#include "app/ActuatorManager.h"
#include "app/DroneApp.h"
#include "app/DroneContext.h"
#include "app/DroneState.h"
#include "app/KeypadController.h"
#include "app/LinkService.h"
#include "app/OledPageController.h"
#include "app/SensorManager.h"
#include "app/TelemetryService.h"
#include "platform/ArduinoClock.h"

#ifndef NODE_ID
#error "NODE_ID must be set in platformio.ini build_flags (e.g. -D NODE_ID=1)"
#endif

namespace {
WindSensorRevC wind(PIN_WIND_RV, PIN_WIND_TMP);
Icm20948Imu imu(0);
Sht31Sensor sht31(Sht31Sensor::kAlternateAddress);
Pa1010dGpsSensor gps(Wire);


HardwareSerial SpsSerial(2);
Sps30UartSensor sps30(
    Sps30UartSensor::Config(&SpsSerial, PIN_SPS_RX, PIN_SPS_TX, 115200, 10000, 1000, 5, true));

MatrixKeypadSensor keypad(KEYPAD_ROWS, KEYPAD_COLS, KEYPAD_MAP, "Drone Keypad");

OledDisplay oled(OledDisplay::Controller::SH1106, 0x3C, "OLED Display");

HardwareSerial LoRaSerial(1);
UartLoRaBridge bridge(LoRaSerial, PIN_LORA_RX, PIN_LORA_TX, 115200);

ISensor *sensors[] = {&imu, &sht31, &gps, &sps30, &wind, &keypad};
IActuator *actuators[] = {&oled};

DroneContext ctx{wind,      imu,
                 sht31,     gps,
                 sps30,     keypad,
                 oled,      bridge,
                 sensors,   sizeof(sensors) / sizeof(sensors[0]),
                 actuators, sizeof(actuators) / sizeof(actuators[0])};


AppState state{};
ArduinoClock clock;
SensorManager sensorManager(ctx);
ActuatorManager actuatorManager(ctx);
TelemetryService telemetry(ctx);
LinkService link(ctx, state, clock, telemetry);
KeypadController keypadController(ctx, state);
OledPageController oledController(ctx, state, clock);
DutyCycleController dutyCycleController(
  DutyCycleController::Config(30000,10000, true)
);
DroneApp app(NODE_ID, ctx, state, clock, sensorManager, actuatorManager,
             telemetry, link, keypadController, oledController, dutyCycleController);
} // namespace

void setup() {
  delay(1000);
  app.setup();
  app.scanI2C();
}

void loop() {
  app.loop();
}
#endif
