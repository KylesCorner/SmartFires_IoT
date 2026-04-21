#pragma once

#include <Arduino.h>
#include "ISensor.h"
#include "IActuator.h"
#include "Icm20948Imu.h"
#include "MatrixKeypadSensor.h"
#include "OledDisplay.h"
#include "Pa1010dGpsSensor.h"
#include "Sht31Sensor.h"
#include "WindSensorRevC.h"
#include "shared/UartLoRaBridge.h"
#include "Sps30UartSensor.h"

struct DroneContext {
  // Concrete hardware
  WindSensorRevC& wind;
  Icm20948Imu& imu;
  Sht31Sensor& sht31;
  Pa1010dGpsSensor& gps;
  Sps30UartSensor& sps30;
  MatrixKeypadSensor& keypad;

  OledDisplay& oled;
  UartLoRaBridge& bridge;

  // Polymorphic registries
  ISensor** sensors;
  size_t numSensors;
  IActuator** actuators;
  size_t numActuators;
};
