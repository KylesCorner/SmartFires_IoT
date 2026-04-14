#pragma once

#include <Arduino.h>
#include "ISensor.h"
#include "IActuator.h"
#include "FlameSensor.h"
#include "Icm20948Imu.h"
#include "LidarLiteV3.h"
#include "MatrixKeypadSensor.h"
#include "OledDisplay.h"
#include "Pa1010dGpsSensor.h"
#include "Sht31Sensor.h"
#include "WindSensorRevC.h"
#include "shared/UartLoRaBridge.h"

struct DroneContext {
  // Concrete hardware
  WindSensorRevC& wind;
  Icm20948Imu& imu;
  FlameSensor& flame;
  Sht31Sensor& sht31;
  Pa1010dGpsSensor& gps;
  LidarLiteV3& lidar;
  MatrixKeypadSensor& keypad;
  OledDisplay& oled;
  UartLoRaBridge& bridge;

  // Polymorphic registries
  ISensor** sensors;
  size_t numSensors;
  IActuator** actuators;
  size_t numActuators;
};
