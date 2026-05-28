#include "platform/SparkfunIcm20948Driver.h"

#include <math.h>

SparkfunIcm20948Driver::SparkfunIcm20948Driver(TwoWire &wire)
    : _wire(wire) {}

bool SparkfunIcm20948Driver::begin(uint8_t ad0Val) {
  _begun = false;

  _imu.begin(_wire, ad0Val);

  if (_imu.status != ICM_20948_Stat_Ok) {
    return false;
  }

  _begun = configureDmp9Dof();
  return _begun;
}

bool SparkfunIcm20948Driver::configureDmp9Dof() {
  bool ok = true;

  // Load the DMP firmware image (~14 KB).
  ok &= (_imu.initializeDMP() == ICM_20948_Stat_Ok);

  // 9DOF Rotation Vector: gyro + accel + mag fusion.
  // Gyro tracks short-term rotation correctly; mag corrects long-term drift.
  // Geomagnetic RV (accel+mag only) anchors to a learned heading and fights
  // back after rotation, making it unsuitable for orientation tracking.
  ok &= (_imu.enableDMPSensor(INV_ICM20948_SENSOR_ROTATION_VECTOR) == ICM_20948_Stat_Ok);

  // ODR: value = (DMP rate / desired rate) - 1.  DMP runs at 225 Hz for Quat9.
  // value=44 → 225/45 = 5 Hz.  Sufficient for a stationary sensor.
  ok &= (_imu.setDMPODRrate(DMP_ODR_Reg_Quat9, 44) == ICM_20948_Stat_Ok);

  ok &= (_imu.enableFIFO()  == ICM_20948_Stat_Ok);
  ok &= (_imu.enableDMP()   == ICM_20948_Stat_Ok);
  ok &= (_imu.resetDMP()    == ICM_20948_Stat_Ok);
  ok &= (_imu.resetFIFO()   == ICM_20948_Stat_Ok);

  return ok;
}

bool SparkfunIcm20948Driver::read(Data &out) {
  out.valid       = false;
  out.headingValid = false;

  if (!_begun) {
    return false;
  }

  icm_20948_DMP_data_t dmpData;
  _imu.readDMPdataFromFIFO(&dmpData);

  // FIFONoDataAvail is the normal "nothing ready yet" status — not an error.
  if (_imu.status == ICM_20948_Stat_FIFONoDataAvail) {
    return false;
  }

  if (_imu.status != ICM_20948_Stat_Ok &&
      _imu.status != ICM_20948_Stat_FIFOMoreDataAvail) {
    return false;
  }

  if ((dmpData.header & DMP_header_bitmap_Quat9) == 0) {
    // Frame arrived but carried no Quat9 data — skip.
    return false;
  }

  // Quaternion components in Q30 format (divide by 2^30).
  // Q1-Q3 are the vector part; Q0 (scalar) is derived.
  double q1 = (double)dmpData.Quat9.Data.Q1 / 1073741824.0;
  double q2 = (double)dmpData.Quat9.Data.Q2 / 1073741824.0;
  double q3 = (double)dmpData.Quat9.Data.Q3 / 1073741824.0;
  double sumSq = q1*q1 + q2*q2 + q3*q3;
  double q0 = (sumSq < 1.0) ? sqrt(1.0 - sumSq) : 0.0;

  // Heading (yaw about vertical) from quaternion — same formula as Quat9 example.
  double yaw = atan2(2.0*(q1*q2 + q0*q3), q0*q0 + q1*q1 - q2*q2 - q3*q3);
  double heading = yaw * (180.0 / M_PI);
  if (heading < 0.0) heading += 360.0;

  // Quat9.Data.Accuracy is a Q12 heading accuracy estimate in degrees.
  // Negative values indicate the sensor has not yet calibrated — reject those.
  int16_t rawAccuracy = dmpData.Quat9.Data.Accuracy;
  if (rawAccuracy < 0) {
    // Not calibrated yet — return false so the sensor keeps its last valid reading.
    return false;
  }

  out.headingDeg      = (float)heading;
  out.headingAccuracy = rawAccuracy;   // caller divides by 4096 to get degrees
  out.headingValid    = true;
  out.valid           = true;
  return true;
}
