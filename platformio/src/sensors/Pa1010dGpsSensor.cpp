#include "sensors/Pa1010dGpsSensor.h"

#include "logging/DebugLogger.h"

#include <stdio.h>

namespace {

const char *gpsPowerModeName(GpsPowerMode mode) {
  switch (mode) {
  case GpsPowerMode::FullPowerContinuous:
    return "FullPowerContinuous";
  case GpsPowerMode::Standby:
    return "Standby";
  case GpsPowerMode::Backup:
    return "Backup";
  case GpsPowerMode::PeriodicStandby:
    return "PeriodicStandby";
  case GpsPowerMode::PeriodicBackup:
    return "PeriodicBackup";
  default:
    return "Unknown";
  }
}

const char *sensorPowerStateName(SensorPowerState state) {
  switch (state) {
  case SensorPowerState::Ready:
    return "Ready";
  case SensorPowerState::Waking:
    return "Waking";
  case SensorPowerState::Sleeping:
    return "Sleeping";
  case SensorPowerState::Error:
    return "Error";
  default:
    return "Unknown";
  }
}

const char *sensorDutyClassName(SensorDutyClass dutyClass) {
  switch (dutyClass) {
  case SensorDutyClass::AlwaysOn:
    return "AlwaysOn";
  case SensorDutyClass::DutyCycled:
    return "DutyCycled";
  case SensorDutyClass::WarmupHeavy:
    return "WarmupHeavy";
  default:
    return "Unknown";
  }
}

} // namespace

Pa1010dGpsSensor::Pa1010dGpsSensor(const Config &cfg, IGpsDriver &driver,
                                   IClock &clock)
    : _cfg(cfg), _driver(driver), _clock(clock) {}

const char *Pa1010dGpsSensor::name() const { return "gps"; }

bool Pa1010dGpsSensor::begin() {
  LOG_INFO("gps",
           "begin_start addr=0x%02X power_mode=%s duty_class=%s "
           "min_sample_period_ms=%lu wake_delay_ms=%lu",
           static_cast<unsigned int>(_cfg.address),
           gpsPowerModeName(_cfg.powerMode),
           sensorDutyClassName(_cfg.dutyClass),
           static_cast<unsigned long>(_cfg.minSamplePeriodMs),
           static_cast<unsigned long>(_cfg.wakeDelayMs));

  _healthy = _driver.begin(_cfg.address);

  if (!_healthy) {
    _state = SensorPowerState::Error;
    _lastSampleMs = 0;

    LOG_ERROR("gps", "begin_failed addr=0x%02X reason=driver_begin_failed",
              static_cast<unsigned int>(_cfg.address));

    return false;
  }

  bool ok = true;

  switch (_cfg.powerMode) {
  case GpsPowerMode::FullPowerContinuous:
    ok = _driver.enterFullPower();
    LOG_DEBUG("gps", "begin_power_cmd mode=%s ok=%u",
              gpsPowerModeName(_cfg.powerMode), ok ? 1 : 0);
    break;

  case GpsPowerMode::Standby:
  case GpsPowerMode::Backup:
    // Do not immediately sleep during begin().
    // These are controlled by the app-level duty-cycle controller.
    ok = true;
    LOG_DEBUG("gps",
              "begin_power_cmd_skipped mode=%s reason=duty_controller_handles_sleep",
              gpsPowerModeName(_cfg.powerMode));
    break;

  case GpsPowerMode::PeriodicStandby:
    ok = _driver.enterPeriodicStandby(_cfg.periodic);
    LOG_DEBUG("gps", "begin_power_cmd mode=%s ok=%u",
              gpsPowerModeName(_cfg.powerMode), ok ? 1 : 0);
    break;

  case GpsPowerMode::PeriodicBackup:
    ok = _driver.enterPeriodicBackup(_cfg.periodic);
    LOG_DEBUG("gps", "begin_power_cmd mode=%s ok=%u",
              gpsPowerModeName(_cfg.powerMode), ok ? 1 : 0);
    break;
  }

  if (!ok) {
    _healthy = false;
    _state = SensorPowerState::Error;
    _lastSampleMs = 0;

    LOG_ERROR("gps", "begin_failed mode=%s reason=power_mode_command_failed",
              gpsPowerModeName(_cfg.powerMode));

    return false;
  }

  _state = SensorPowerState::Ready;
  _lastSampleMs = 0;

  LOG_INFO("gps", "begin_ok state=%s healthy=%u",
           sensorPowerStateName(_state), _healthy ? 1 : 0);

  return true;
}

bool Pa1010dGpsSensor::wake() {
  if (!_healthy) {
    LOG_WARN("gps", "wake_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));
    return false;
  }

  LOG_DEBUG("gps", "wake_start mode=%s state=%s",
            gpsPowerModeName(_cfg.powerMode), sensorPowerStateName(_state));

  bool ok = true;

  switch (_cfg.powerMode) {
  case GpsPowerMode::FullPowerContinuous:
    ok = _driver.enterFullPower();
    break;

  case GpsPowerMode::Standby:
    ok = _driver.enterFullPower();
    break;

  case GpsPowerMode::Backup:
    ok = _driver.wakeFromBackup();
    break;

  case GpsPowerMode::PeriodicStandby:
  case GpsPowerMode::PeriodicBackup:
    // Periodic mode is autonomous. Do not send PMTK225,0 here,
    // because that exits periodic mode.
    ok = true;
    break;
  }

  if (!ok) {
    _state = SensorPowerState::Error;
    _healthy = false;

    LOG_ERROR("gps", "wake_failed mode=%s state=%s",
              gpsPowerModeName(_cfg.powerMode), sensorPowerStateName(_state));

    return false;
  }

  if (_cfg.dutyClass == SensorDutyClass::AlwaysOn ||
      _cfg.powerMode == GpsPowerMode::PeriodicStandby ||
      _cfg.powerMode == GpsPowerMode::PeriodicBackup) {
    _state = SensorPowerState::Ready;

    LOG_DEBUG("gps", "wake_ok immediate_ready=1 mode=%s state=%s",
              gpsPowerModeName(_cfg.powerMode), sensorPowerStateName(_state));

    return true;
  }

  _wakeStartMs = _clock.millis();
  _state = SensorPowerState::Waking;

  LOG_DEBUG("gps", "wake_ok state=%s wake_start_ms=%lu wake_delay_ms=%lu",
            sensorPowerStateName(_state),
            static_cast<unsigned long>(_wakeStartMs),
            static_cast<unsigned long>(_cfg.wakeDelayMs));

  return true;
}

bool Pa1010dGpsSensor::sleep() {
  if (!_healthy) {
    LOG_WARN("gps", "sleep_reject reason=not_healthy state=%s",
             sensorPowerStateName(_state));
    return false;
  }

  LOG_DEBUG("gps", "sleep_start mode=%s state=%s",
            gpsPowerModeName(_cfg.powerMode), sensorPowerStateName(_state));

  switch (_cfg.powerMode) {
  case GpsPowerMode::FullPowerContinuous:
    _state = SensorPowerState::Ready;

    LOG_DEBUG("gps",
              "sleep_skipped mode=%s state=%s reason=full_power_continuous",
              gpsPowerModeName(_cfg.powerMode), sensorPowerStateName(_state));

    return true;

  case GpsPowerMode::Standby:
    if (!_driver.enterStandby()) {
      _state = SensorPowerState::Error;
      _healthy = false;

      LOG_ERROR("gps", "sleep_failed mode=%s reason=enter_standby_failed",
                gpsPowerModeName(_cfg.powerMode));

      return false;
    }

    _state = SensorPowerState::Sleeping;

    LOG_DEBUG("gps", "sleep_ok mode=%s state=%s",
              gpsPowerModeName(_cfg.powerMode), sensorPowerStateName(_state));

    return true;

  case GpsPowerMode::Backup:
    if (!_driver.enterBackup()) {
      _state = SensorPowerState::Error;
      _healthy = false;

      LOG_ERROR("gps", "sleep_failed mode=%s reason=enter_backup_failed",
                gpsPowerModeName(_cfg.powerMode));

      return false;
    }

    _state = SensorPowerState::Sleeping;

    LOG_DEBUG("gps", "sleep_ok mode=%s state=%s",
              gpsPowerModeName(_cfg.powerMode), sensorPowerStateName(_state));

    return true;

  case GpsPowerMode::PeriodicStandby:
  case GpsPowerMode::PeriodicBackup:
    // Already handled internally by PA1010D.
    // Keep the software sensor ready so service()/poll() continues.
    _state = SensorPowerState::Ready;

    LOG_DEBUG("gps",
              "sleep_skipped mode=%s state=%s reason=periodic_autonomous",
              gpsPowerModeName(_cfg.powerMode), sensorPowerStateName(_state));

    return true;
  }

  LOG_ERROR("gps", "sleep_failed mode=%s reason=unhandled_power_mode",
            gpsPowerModeName(_cfg.powerMode));

  return false;
}

bool Pa1010dGpsSensor::service() {
  if (!_healthy) {
    LOG_TRACE("gps", "service_skip reason=not_healthy state=%s",
              sensorPowerStateName(_state));
    return false;
  }

  if (!_driver.poll()) {
    LOG_WARN("gps", "service_poll_failed state=%s", sensorPowerStateName(_state));
    return false;
  }

  if (_state == SensorPowerState::Waking &&
      _clock.millis() - _wakeStartMs >= _cfg.wakeDelayMs) {
    const uint32_t elapsedMs = _clock.millis() - _wakeStartMs;

    _state = SensorPowerState::Ready;

    LOG_INFO("gps", "wake_complete elapsed_ms=%lu state=%s",
             static_cast<unsigned long>(elapsedMs), sensorPowerStateName(_state));
  }

  return _state == SensorPowerState::Ready;
}

bool Pa1010dGpsSensor::sample() {
  if (!ready()) {
    LOG_TRACE("gps",
              "sample_skip reason=not_ready healthy=%u state=%s "
              "elapsed_since_last_ms=%lu min_sample_period_ms=%lu",
              _healthy ? 1 : 0, sensorPowerStateName(_state),
              static_cast<unsigned long>(_clock.millis() - _lastSampleMs),
              static_cast<unsigned long>(_cfg.minSamplePeriodMs));
    return false;
  }

  IGpsDriver::Data data;

  if (!_driver.read(data)) {
    _reading.valid = false;

    LOG_WARN("gps", "sample_failed reason=driver_read_failed");

    return false;
  }

  static_cast<IGpsDriver::Data &>(_reading) = data;

  _reading.valid = data.fix;
  _reading.timestampMs = _clock.millis();
  _lastSampleMs = _clock.millis();

  return true;
}

bool Pa1010dGpsSensor::ready() const {
  return _healthy && _state == SensorPowerState::Ready &&
         _clock.millis() - _lastSampleMs >= _cfg.minSamplePeriodMs;
}

bool Pa1010dGpsSensor::healthy() const { return _healthy; }

SensorPowerState Pa1010dGpsSensor::powerState() const { return _state; }

SensorDutyClass Pa1010dGpsSensor::dutyClass() const { return _cfg.dutyClass; }

bool Pa1010dGpsSensor::reset() {
  LOG_WARN("gps", "reset_start state=%s healthy=%u",
           sensorPowerStateName(_state), _healthy ? 1 : 0);

  _state = SensorPowerState::Off;
  _healthy = false;
  _reading = Reading{};
  _wakeStartMs = 0;
  _lastSampleMs = 0;

  if (!_driver.reset()) {
    _state = SensorPowerState::Error;
    LOG_ERROR("gps", "reset_failed reason=driver_reset_failed");
    return false;
  }

  return begin();
}

const Pa1010dGpsSensor::Reading &Pa1010dGpsSensor::reading() const {
  return _reading;
}

void Pa1010dGpsSensor::fillSnapshot(SensorSnapshot &snap) const {
  if (!_reading.valid) {
    LOG_TRACE("gps", "snapshot_skip reason=invalid_reading");
    return;
  }

  snap.latDeg = _reading.latitudeDeg;
  snap.lonDeg = _reading.longitudeDeg;
  snap.sensorFlags |= 0x04; // GPS

  LOG_TRACE("gps", "snapshot_fill flags=0x%04X",
            static_cast<unsigned int>(snap.sensorFlags));
}

const void *Pa1010dGpsSensor::readingData() const { return &_reading; }

size_t Pa1010dGpsSensor::readingSize() const { return sizeof(Reading); }

size_t Pa1010dGpsSensor::writeTelemetry(char *out, size_t maxLen) const {
  if (!out || maxLen == 0) return 0;

  int n = snprintf(
      out, maxLen,
      "gps,fix=%u,fixq=%u,sats=%u,lat=%.6f,lon=%.6f,alt=%.2f,t=%02u:%02u:%02u,valid=%u,t_ms=%lu",
      _reading.fix ? 1 : 0,
      _reading.fixQuality,
      _reading.satellites,
      _reading.latitudeDeg,
      _reading.longitudeDeg,
      _reading.altitudeM,
      _reading.hour,
      _reading.minute,
      _reading.second,
      _reading.valid ? 1 : 0,
      static_cast<unsigned long>(_reading.timestampMs));

  if (n < 0) return 0;
  return static_cast<size_t>(n) >= maxLen ? maxLen - 1 : static_cast<size_t>(n);
}
