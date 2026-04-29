#pragma once

#include <stddef.h>
#include <stdint.h>

class ITdmaRadioDriver {
public:
  struct ReceivedPacket {
    uint8_t from = 0;
    uint8_t data[255] = {};
    uint8_t len = 0;
    int8_t rssi = 0;
  };

  virtual ~ITdmaRadioDriver() = default;

  virtual bool begin() = 0;
  virtual bool sendToWait(const uint8_t *data, uint8_t len, uint8_t to) = 0;

  virtual bool available() = 0;
  virtual bool receive(ReceivedPacket &out) = 0;

  virtual bool healthy() const = 0;
};
