#pragma once
#include <Arduino.h>
#include "IActuator.h"

/**
 * Passive buzzer driver for Arduino Uno/Nano using tone().
 *
 * Features:
 * - Non-blocking beep scheduling (no delay() inside)
 * - Optional repeating beep pattern
 *
 * Wiring:
 * - Signal pin -> buzzer S (or +) through resistor if recommended by module
 * - GND -> buzzer -
 *
 * Note:
 * - tone() uses a hardware timer on AVR. It can conflict with some libraries.
 */
class PassiveBuzzer : public IActuator {
public:
  explicit PassiveBuzzer(uint8_t pin, const char* actuatorName = "Buzzer")
    : _pin(pin), _name(actuatorName) {}

  const char* name() const override { return _name; }

  bool begin() override {
    pinMode(_pin, OUTPUT);
    noTone(_pin);
    _healthy = true;
    _state = State::Idle;
    _repeat = false;
    return true;
  }

  bool healthy() const override { return _healthy; }

  // Call in loop() often
  void update() override {
    const uint32_t now = millis();

    switch (_state) {
      case State::Idle:
        return;

      case State::On:
        if (elapsed(now, _phaseStartMs) >= _onMs) {
          noTone(_pin);
          _state = State::Off;
          _phaseStartMs = now;
        }
        return;

      case State::Off:
        if (elapsed(now, _phaseStartMs) >= _offMs) {
          if (_repeat) {
            startTone(now);
          } else {
            _state = State::Idle;
          }
        }
        return;
    }
  }

  // One-shot beep: plays frequency for onMs, then stops.
  void beep(uint16_t frequencyHz, uint16_t onMs) {
    _freqHz = frequencyHz;
    _onMs = onMs;
    _offMs = 0;
    _repeat = false;

    const uint32_t now = millis();
    startTone(now);
  }

  // Beep pattern: ON for onMs, OFF for offMs, repeat (until stop()).
  void startBeepPattern(uint16_t frequencyHz, uint16_t onMs, uint16_t offMs) {
    _freqHz = frequencyHz;
    _onMs = onMs;
    _offMs = offMs;
    _repeat = true;

    const uint32_t now = millis();
    startTone(now);
  }

  void stop() {
    noTone(_pin);
    _state = State::Idle;
    _repeat = false;
  }

  bool isPlaying() const { return _state != State::Idle; }

private:
  enum class State : uint8_t { Idle, On, Off };

  static uint32_t elapsed(uint32_t now, uint32_t start) {
    return now - start; // millis rollover-safe with unsigned arithmetic
  }

  void startTone(uint32_t now) {
    // tone(pin, freq) continues until noTone() is called
    tone(_pin, _freqHz);
    _state = State::On;
    _phaseStartMs = now;
  }

  uint8_t _pin;
  const char* _name;

  bool _healthy = true;

  State _state = State::Idle;
  bool _repeat = false;

  uint16_t _freqHz = 2000;
  uint16_t _onMs = 100;
  uint16_t _offMs = 100;

  uint32_t _phaseStartMs = 0;
};