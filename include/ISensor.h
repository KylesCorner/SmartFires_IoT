#pragma once
#include <Arduino.h>

/**
 * Generic sensor interface.
 *
 * Notes for embedded:
 * - Avoid dynamic allocation.
 * - Keep methods small and non-blocking where possible.
 * - Use return codes instead of exceptions.
 */
class ISensor {
public:
  virtual ~ISensor() = default;

  // Human-readable name (stored in flash if you want later; keep simple for now)
  virtual const char* name() const = 0;

  // Initialize hardware / library state.
  // Return true on success.
  virtual bool begin() = 0;

  // Whether the sensor is ready to provide data right now
  // (useful for warmup-time devices).
  virtual bool ready() const = 0;

  // Request an update (kick off a read). For simple sensors this can do the read immediately.
  // Return true if a new reading was captured/updated.
  virtual bool sample() = 0;

  // Age of last reading in milliseconds. If no reading yet, return UINT32_MAX.
  virtual uint32_t ageMs() const = 0;

  // Optional: mark sensor unhealthy on repeated failures, wiring issues, etc.
  virtual bool healthy() const = 0;
};