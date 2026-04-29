#include "platform/SparkfunIcm20948Driver.h"

SparkfunIcm20948Driver::SparkfunIcm20948Driver(TwoWire &wire)
    : _wire(wire) {}

bool SparkfunIcm20948Driver::begin(uint8_t ad0Val) {
  _begun = false;

  _imu.begin(_wire, ad0Val);

  if (_imu.status != ICM_20948_Stat_Ok) {
    return false;
  }

  _begun = configureNormalMode();
  return _begun;
}

bool SparkfunIcm20948Driver::configureNormalMode() {
  if (_imu.sleep(false) != ICM_20948_Stat_Ok) {
    return false;
  }

  if (_imu.lowPower(false) != ICM_20948_Stat_Ok) {
    return false;
  }

  if (_imu.setSampleMode(ICM_20948_Internal_Acc,
                         ICM_20948_Sample_Mode_Continuous) !=
      ICM_20948_Stat_Ok) {
    return false;
  }

  if (_imu.setSampleMode(ICM_20948_Internal_Gyr,
                         ICM_20948_Sample_Mode_Continuous) !=
      ICM_20948_Stat_Ok) {
    return false;
  }

  return true;
}

bool SparkfunIcm20948Driver::read(Data &out) {
  out.valid = false;

  if (!_begun) {
    return false;
  }

  if (!_imu.dataReady()) {
    return false;
  }

  _imu.getAGMT();

  out.accelX = _imu.accX();
  out.accelY = _imu.accY();
  out.accelZ = _imu.accZ();

  out.gyroX = _imu.gyrX();
  out.gyroY = _imu.gyrY();
  out.gyroZ = _imu.gyrZ();

  out.magX = _imu.magX();
  out.magY = _imu.magY();
  out.magZ = _imu.magZ();

  out.valid = true;
  return true;
}
