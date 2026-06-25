// ---
// description: Generic send/receive radio abstraction that hardware-specific LoRa drivers implement.
// role: interface
// ---
#pragma once

#include <stddef.h>
#include <stdint.h>

class IRadio {
public:
  virtual ~IRadio() = default;

  virtual bool begin() = 0;
  virtual bool available() = 0;

  virtual bool send(const uint8_t *data, size_t len) = 0;
  virtual size_t receive(uint8_t *out, size_t maxLen) = 0;

  virtual bool healthy() const = 0;
};
