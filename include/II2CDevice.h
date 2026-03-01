#pragma once
#include <Arduino.h>
#include <Wire.h>

// Lightweight status codes (Arduino Wire has limited error reporting)
enum class I2CStatus : uint8_t {
  Ok = 0,
  NackOnAddress,
  NackOnData,
  BusError,
  BufferTooSmall,
  Timeout,
  UnknownError
};

class II2CDevice {
public:
  virtual ~II2CDevice() = default;

  // Human-readable identifier
  virtual const char* name() const = 0;

  // 7-bit I2C address (0x00..0x7F). Do NOT include R/W bit.
  virtual uint8_t address() const = 0;

  // Initialize device; Wire should already be started, but we accept TwoWire for flexibility.
  virtual bool begin(TwoWire& wire = Wire) = 0;

  // Basic liveness check (typically: address ACK)
  virtual bool ping() = 0;

  // Optional: health flag (e.g., failed reads)
  virtual bool healthy() const = 0;
};