// ---
// description: Adafruit_SHT31 I2C temperature/humidity driver implementing
// ISht31Driver, with detailed transaction diagnostics and retrying reads.
// role: implementation
// ---
#pragma once

#include "drivers/ISht31Driver.h"

#include <Adafruit_SHT31.h>
#include <stdint.h>

class AdafruitSht31Driver final : public ISht31Driver {
public:
  enum class ReadError : uint8_t {
    None = 0,

    CommandBufferError,
    CommandAddressNack,
    CommandDataNack,
    CommandOtherError,

    ReadAddressNack,
    ShortRead,

    TemperatureCrc,
    HumidityCrc,

    TemperatureRange,
    HumidityRange,
  };

  struct Diagnostics {
    uint32_t successfulReads = 0;
    uint32_t recoveredReads = 0;
    uint32_t failedReads = 0;
    uint32_t failedAttempts = 0;

    uint32_t commandBufferErrors = 0;
    uint32_t commandAddressNacks = 0;
    uint32_t commandDataNacks = 0;
    uint32_t commandOtherErrors = 0;

    uint32_t readAddressNacks = 0;
    uint32_t shortReads = 0;

    uint32_t temperatureCrcFailures = 0;
    uint32_t humidityCrcFailures = 0;

    uint32_t temperatureRangeFailures = 0;
    uint32_t humidityRangeFailures = 0;
  };

  bool begin(uint8_t address) override;

  bool read(float &temperatureC, float &humidityPct) override;

  float readTemperatureC() override;
  float readHumidityPct() override;

  ReadError lastError() const;
  const Diagnostics &diagnostics() const;

  static const char *errorName(ReadError error);

private:
  bool readOnce(float &temperatureC,
                float &humidityPct,
                ReadError &error,
                uint8_t attempt);

  void recordAttemptFailure(ReadError error);

  uint8_t _address = SHT31_DEFAULT_ADDR;
  ReadError _lastError = ReadError::None;
  Diagnostics _diagnostics;

  Adafruit_SHT31 _sht31;
};