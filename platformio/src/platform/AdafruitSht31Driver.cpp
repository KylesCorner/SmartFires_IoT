// ---
// description: Adafruit_SHT31 I2C temperature/humidity driver implementing
// ISht31Driver, with detailed transaction diagnostics and retrying reads.
// role: implementation
// ---
#include "platform/AdafruitSht31Driver.h"

#include "logging/DebugLogger.h"
#include "platform/ResetDiagnostics.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

namespace {

constexpr float kMinTempC = -40.0f;
constexpr float kMaxTempC = 125.0f;
constexpr float kMinHumidityPct = 0.0f;
constexpr float kMaxHumidityPct = 100.0f;

constexpr uint8_t kReadAttempts = 2;
constexpr uint16_t kRetryDelayMs = 5;

// SHT31 high-repeatability single-shot measurement,
// clock stretching disabled.
constexpr uint16_t kMeasurementCommand = 0x2400;
constexpr uint16_t kMeasurementDelayMs = 20;

constexpr uint8_t kResponseSize = 6;

bool validTemperatureC(float value) {
  return isfinite(value) &&
         value >= kMinTempC &&
         value <= kMaxTempC;
}

bool validHumidityPct(float value) {
  return isfinite(value) &&
         value >= kMinHumidityPct &&
         value <= kMaxHumidityPct;
}

// SHT3x CRC-8:
//   Initial value: 0xFF
//   Polynomial:    0x31
//   Final XOR:     0x00
uint8_t calculateCrc8(const uint8_t *data, size_t len) {
  constexpr uint8_t kPolynomial = 0x31;

  uint8_t crc = 0xFF;

  for (size_t byteIndex = 0; byteIndex < len; ++byteIndex) {
    crc ^= data[byteIndex];

    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (crc & 0x80u) {
        crc = static_cast<uint8_t>((crc << 1u) ^ kPolynomial);
      } else {
        crc = static_cast<uint8_t>(crc << 1u);
      }
    }
  }

  return crc;
}

// Match the conversion used by Adafruit_SHT31 so changing to the
// diagnostic transaction does not alter the normal output values.
float decodeTemperatureC(const uint8_t *data) {
  int32_t raw =
      static_cast<int32_t>(
          (static_cast<uint16_t>(data[0]) << 8u) |
          static_cast<uint16_t>(data[1]));

  raw = ((4375L * raw) >> 14) - 4500L;

  return static_cast<float>(raw) / 100.0f;
}

float decodeHumidityPct(const uint8_t *data) {
  uint32_t raw =
      (static_cast<uint32_t>(data[0]) << 8u) |
      static_cast<uint32_t>(data[1]);

  raw = (625UL * raw) >> 12;

  return static_cast<float>(raw) / 100.0f;
}

} // namespace

bool AdafruitSht31Driver::begin(uint8_t address) {
  _address = address;
  _lastError = ReadError::None;

  const bool ok = _sht31.begin(address);

  if (ok) {
    _sht31.heater(false);

    LOG_INFO(
        "sht31",
        "driver_begin_ok addr=0x%02X",
        static_cast<unsigned int>(_address));
  } else {
    LOG_ERROR(
        "sht31",
        "driver_begin_failed addr=0x%02X",
        static_cast<unsigned int>(_address));
  }

  return ok;
}

bool AdafruitSht31Driver::read(float &temperatureC, float &humidityPct) {
  // Marks the shared-I2C SHT31 transaction so a WDT reboot that hangs here is
  // attributed to ZONE_I2C_SHT31 (see ResetDiagnostics).
  ReadError finalError = ReadError::None;
  ResetDiagnostics::ZoneScope zone(ResetDiagnostics::ZONE_I2C_SHT31);

  for (uint8_t attempt = 0; attempt < kReadAttempts; ++attempt) {
    float temp = NAN;
    float humidity = NAN;
    ReadError error = ReadError::None;

    if (readOnce(temp, humidity, error, attempt)) {
      temperatureC = temp;
      humidityPct = humidity;

      ++_diagnostics.successfulReads;

      if (attempt > 0) {
        ++_diagnostics.recoveredReads;

        LOG_INFO(
            "sht31",
            "read_recovered attempt=%u/%u temp_c=%.2f humidity_pct=%.2f "
            "recovered_total=%lu",
            static_cast<unsigned int>(attempt + 1u),
            static_cast<unsigned int>(kReadAttempts),
            temp,
            humidity,
            static_cast<unsigned long>(_diagnostics.recoveredReads));
      }

      _lastError = ReadError::None;
      return true;
    }

    finalError = error;
    recordAttemptFailure(error);

    if (attempt + 1u < kReadAttempts) {
      delay(kRetryDelayMs);
    }
  }

  ++_diagnostics.failedReads;
  _lastError = finalError;

  temperatureC = NAN;
  humidityPct = NAN;

  LOG_WARN(
      "sht31",
      "read_failed attempts=%u last_error=%s "
      "failed_reads=%lu failed_attempts=%lu "
      "cmd_addr_nack=%lu cmd_data_nack=%lu "
      "read_addr_nack=%lu short_read=%lu "
      "temp_crc=%lu humidity_crc=%lu "
      "temp_range=%lu humidity_range=%lu",
      static_cast<unsigned int>(kReadAttempts),
      errorName(finalError),
      static_cast<unsigned long>(_diagnostics.failedReads),
      static_cast<unsigned long>(_diagnostics.failedAttempts),
      static_cast<unsigned long>(_diagnostics.commandAddressNacks),
      static_cast<unsigned long>(_diagnostics.commandDataNacks),
      static_cast<unsigned long>(_diagnostics.readAddressNacks),
      static_cast<unsigned long>(_diagnostics.shortReads),
      static_cast<unsigned long>(_diagnostics.temperatureCrcFailures),
      static_cast<unsigned long>(_diagnostics.humidityCrcFailures),
      static_cast<unsigned long>(_diagnostics.temperatureRangeFailures),
      static_cast<unsigned long>(_diagnostics.humidityRangeFailures));

  return false;
}

bool AdafruitSht31Driver::readOnce(float &temperatureC,
                                   float &humidityPct,
                                   ReadError &error,
                                   uint8_t attempt) {
  error = ReadError::None;

  const uint8_t command[2] = {
      static_cast<uint8_t>(kMeasurementCommand >> 8u),
      static_cast<uint8_t>(kMeasurementCommand & 0xFFu),
  };

  // --------------------------------------------------------------------------
  // Send measurement command
  // --------------------------------------------------------------------------

  Wire.beginTransmission(_address);

  const size_t queued = Wire.write(command, sizeof(command));

  if (queued != sizeof(command)) {
    // Complete/abort the transmission so Wire's internal transmission state
    // does not remain active.
    (void)Wire.endTransmission();

    error = ReadError::CommandBufferError;

    LOG_WARN(
        "sht31",
        "read_attempt_failed attempt=%u/%u "
        "reason=%s queued=%u expected=%u",
        static_cast<unsigned int>(attempt + 1u),
        static_cast<unsigned int>(kReadAttempts),
        errorName(error),
        static_cast<unsigned int>(queued),
        static_cast<unsigned int>(sizeof(command)));

    return false;
  }

  // SAMD Wire return values:
  //   0 = success
  //   1 = data too long
  //   2 = address NACK
  //   3 = data NACK
  //   4 = other error
  const uint8_t transmissionStatus = Wire.endTransmission();

  if (transmissionStatus != 0) {
    switch (transmissionStatus) {
    case 1:
      error = ReadError::CommandBufferError;
      break;

    case 2:
      error = ReadError::CommandAddressNack;
      break;

    case 3:
      error = ReadError::CommandDataNack;
      break;

    default:
      error = ReadError::CommandOtherError;
      break;
    }

    LOG_WARN(
        "sht31",
        "read_attempt_failed attempt=%u/%u "
        "reason=%s wire_status=%u addr=0x%02X",
        static_cast<unsigned int>(attempt + 1u),
        static_cast<unsigned int>(kReadAttempts),
        errorName(error),
        static_cast<unsigned int>(transmissionStatus),
        static_cast<unsigned int>(_address));

    return false;
  }

  // High-repeatability conversion takes less than this under normal
  // operating conditions.
  delay(kMeasurementDelayMs);

  // --------------------------------------------------------------------------
  // Read six-byte result
  // --------------------------------------------------------------------------

  const uint8_t received = Wire.requestFrom(
      _address,
      static_cast<size_t>(kResponseSize));

  if (received == 0) {
    error = ReadError::ReadAddressNack;

    LOG_WARN(
        "sht31",
        "read_attempt_failed attempt=%u/%u "
        "reason=%s received=0 expected=%u addr=0x%02X",
        static_cast<unsigned int>(attempt + 1u),
        static_cast<unsigned int>(kReadAttempts),
        errorName(error),
        static_cast<unsigned int>(kResponseSize),
        static_cast<unsigned int>(_address));

    return false;
  }

  if (received != kResponseSize) {
    while (Wire.available() > 0) {
      (void)Wire.read();
    }

    error = ReadError::ShortRead;

    LOG_WARN(
        "sht31",
        "read_attempt_failed attempt=%u/%u "
        "reason=%s received=%u expected=%u",
        static_cast<unsigned int>(attempt + 1u),
        static_cast<unsigned int>(kReadAttempts),
        errorName(error),
        static_cast<unsigned int>(received),
        static_cast<unsigned int>(kResponseSize));

    return false;
  }

  uint8_t response[kResponseSize] = {};

  for (uint8_t i = 0; i < kResponseSize; ++i) {
    if (Wire.available() <= 0) {
      error = ReadError::ShortRead;

      LOG_WARN(
          "sht31",
          "read_attempt_failed attempt=%u/%u "
          "reason=%s buffer_index=%u received=%u",
          static_cast<unsigned int>(attempt + 1u),
          static_cast<unsigned int>(kReadAttempts),
          errorName(error),
          static_cast<unsigned int>(i),
          static_cast<unsigned int>(received));

      return false;
    }

    response[i] = static_cast<uint8_t>(Wire.read());
  }

  // --------------------------------------------------------------------------
  // Validate sensor CRC bytes
  // --------------------------------------------------------------------------

  const uint8_t expectedTemperatureCrc =
      calculateCrc8(response, 2);

  if (response[2] != expectedTemperatureCrc) {
    error = ReadError::TemperatureCrc;

    LOG_WARN(
        "sht31",
        "read_attempt_failed attempt=%u/%u "
        "reason=%s raw_temp=%02X%02X received_crc=0x%02X "
        "expected_crc=0x%02X",
        static_cast<unsigned int>(attempt + 1u),
        static_cast<unsigned int>(kReadAttempts),
        errorName(error),
        static_cast<unsigned int>(response[0]),
        static_cast<unsigned int>(response[1]),
        static_cast<unsigned int>(response[2]),
        static_cast<unsigned int>(expectedTemperatureCrc));

    return false;
  }

  const uint8_t expectedHumidityCrc =
      calculateCrc8(response + 3, 2);

  if (response[5] != expectedHumidityCrc) {
    error = ReadError::HumidityCrc;

    LOG_WARN(
        "sht31",
        "read_attempt_failed attempt=%u/%u "
        "reason=%s raw_humidity=%02X%02X received_crc=0x%02X "
        "expected_crc=0x%02X",
        static_cast<unsigned int>(attempt + 1u),
        static_cast<unsigned int>(kReadAttempts),
        errorName(error),
        static_cast<unsigned int>(response[3]),
        static_cast<unsigned int>(response[4]),
        static_cast<unsigned int>(response[5]),
        static_cast<unsigned int>(expectedHumidityCrc));

    return false;
  }

  // --------------------------------------------------------------------------
  // Decode and range-check
  // --------------------------------------------------------------------------

  const float temp = decodeTemperatureC(response);
  const float humidity = decodeHumidityPct(response + 3);

  if (!validTemperatureC(temp)) {
    error = ReadError::TemperatureRange;

    LOG_WARN(
        "sht31",
        "read_attempt_failed attempt=%u/%u "
        "reason=%s temp_c=%.2f",
        static_cast<unsigned int>(attempt + 1u),
        static_cast<unsigned int>(kReadAttempts),
        errorName(error),
        temp);

    return false;
  }

  if (!validHumidityPct(humidity)) {
    error = ReadError::HumidityRange;

    LOG_WARN(
        "sht31",
        "read_attempt_failed attempt=%u/%u "
        "reason=%s humidity_pct=%.2f",
        static_cast<unsigned int>(attempt + 1u),
        static_cast<unsigned int>(kReadAttempts),
        errorName(error),
        humidity);

    return false;
  }

  temperatureC = temp;
  humidityPct = humidity;

  return true;
}

void AdafruitSht31Driver::recordAttemptFailure(ReadError error) {
  ++_diagnostics.failedAttempts;

  switch (error) {
  case ReadError::CommandBufferError:
    ++_diagnostics.commandBufferErrors;
    break;

  case ReadError::CommandAddressNack:
    ++_diagnostics.commandAddressNacks;
    break;

  case ReadError::CommandDataNack:
    ++_diagnostics.commandDataNacks;
    break;

  case ReadError::CommandOtherError:
    ++_diagnostics.commandOtherErrors;
    break;

  case ReadError::ReadAddressNack:
    ++_diagnostics.readAddressNacks;
    break;

  case ReadError::ShortRead:
    ++_diagnostics.shortReads;
    break;

  case ReadError::TemperatureCrc:
    ++_diagnostics.temperatureCrcFailures;
    break;

  case ReadError::HumidityCrc:
    ++_diagnostics.humidityCrcFailures;
    break;

  case ReadError::TemperatureRange:
    ++_diagnostics.temperatureRangeFailures;
    break;

  case ReadError::HumidityRange:
    ++_diagnostics.humidityRangeFailures;
    break;

  case ReadError::None:
  default:
    break;
  }
}

AdafruitSht31Driver::ReadError
AdafruitSht31Driver::lastError() const {
  return _lastError;
}

const AdafruitSht31Driver::Diagnostics &
AdafruitSht31Driver::diagnostics() const {
  return _diagnostics;
}

const char *
AdafruitSht31Driver::errorName(ReadError error) {
  switch (error) {
  case ReadError::None:
    return "none";

  case ReadError::CommandBufferError:
    return "command_buffer_error";

  case ReadError::CommandAddressNack:
    return "command_address_nack";

  case ReadError::CommandDataNack:
    return "command_data_nack";

  case ReadError::CommandOtherError:
    return "command_other_error";

  case ReadError::ReadAddressNack:
    return "read_address_nack";

  case ReadError::ShortRead:
    return "short_read";

  case ReadError::TemperatureCrc:
    return "temperature_crc";

  case ReadError::HumidityCrc:
    return "humidity_crc";

  case ReadError::TemperatureRange:
    return "temperature_range";

  case ReadError::HumidityRange:
    return "humidity_range";

  default:
    return "unknown";
  }
}

float AdafruitSht31Driver::readTemperatureC() {
  float temp = NAN;
  float humidity = NAN;

  if (!read(temp, humidity)) {
    return NAN;
  }

  return temp;
}

float AdafruitSht31Driver::readHumidityPct() {
  float temp = NAN;
  float humidity = NAN;

  if (!read(temp, humidity)) {
    return NAN;
  }

  return humidity;
}