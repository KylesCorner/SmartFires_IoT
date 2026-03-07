#pragma once
#include <Arduino.h>
#include <Servo.h>
#include "IActuator.h"

/**
 * MicroServo actuator driver (3-pin SG90-style).
 *
 * Wiring:
 * - Brown/Black: GND (shared with Arduino GND)
 * - Red: +5V (prefer external 5V supply; do NOT pull servo power from Uno 5V if it browns out)
 * - Orange/Yellow: Signal -> Arduino digital pin
 *
 * Notes:
 * - Use update() for smooth motion (rate-limited).
 * - For immediate moves, set maxDegPerSec very high or call setAngleImmediate().
 */
class MicroServo : public IActuator {
public:
  MicroServo(uint8_t signalPin,
             const char* actuatorName = "Servo",
             uint8_t minAngleDeg = 0,
             uint8_t maxAngleDeg = 180,
             uint16_t minPulseUs = 544,
             uint16_t maxPulseUs = 2400)
    : _pin(signalPin),
      _name(actuatorName),
      _minAngle(minAngleDeg),
      _maxAngle(maxAngleDeg),
      _minPulseUs(minPulseUs),
      _maxPulseUs(maxPulseUs) {}

  const char* name() const override { return _name; }

  bool begin() override {
    _servo.attach(_pin, _minPulseUs, _maxPulseUs);
    _healthy = true;

    _currentAngle = clampAngle(_currentAngle);
    _targetAngle = _currentAngle;
    _servo.write(_currentAngle);

    _lastUpdateMs = millis();
    return true;
  }

  bool healthy() const override { return _healthy; }

  /**
   * Call frequently in loop(). Moves the servo toward target at a limited rate.
   */
  void update() override {
    const uint32_t now = millis();
    const uint32_t dtMs = now - _lastUpdateMs;
    if (dtMs == 0) return;
    _lastUpdateMs = now;

    if (_currentAngle == _targetAngle) return;

    // rate limiting
    const float dtSec = dtMs / 1000.0f;
    const float maxStep = _maxDegPerSec * dtSec;

    float diff = (float)_targetAngle - (float)_currentAngle;
    if (fabs(diff) <= maxStep) {
      _currentAngle = _targetAngle;
    } else {
      _currentAngle += (diff > 0) ? (int)round(maxStep) : -(int)round(maxStep);
    }

    _currentAngle = clampAngle(_currentAngle);
    _servo.write(_currentAngle);
  }

  /**
   * Set new target angle. Actual motion occurs over time via update().
   */
  void setAngle(uint8_t angleDeg) {
    _targetAngle = clampAngle(angleDeg);
  }

  /**
   * Move immediately (no smoothing). Useful for initialization or hard moves.
   */
  void setAngleImmediate(uint8_t angleDeg) {
    _targetAngle = clampAngle(angleDeg);
    _currentAngle = _targetAngle;
    _servo.write(_currentAngle);
  }

  uint8_t currentAngle() const { return _currentAngle; }
  uint8_t targetAngle() const { return _targetAngle; }

  /**
   * Set max movement speed in degrees per second.
   * Example: 180 deg/s ~ full sweep in ~1s.
   */
  void setMaxDegPerSec(float degPerSec) {
    if (degPerSec < 1.0f) degPerSec = 1.0f;
    _maxDegPerSec = degPerSec;
  }

  /**
   * Optional: detach to save power / free timer resources.
   */
  void detach() { _servo.detach(); }
  void attach() { if (!_servo.attached()) _servo.attach(_pin, _minPulseUs, _maxPulseUs); }

private:
  uint8_t clampAngle(int angle) const {
    if (angle < _minAngle) return _minAngle;
    if (angle > _maxAngle) return _maxAngle;
    return (uint8_t)angle;
  }

  uint8_t _pin;
  const char* _name;

  Servo _servo;
  bool _healthy = true;

  uint8_t _minAngle = 0;
  uint8_t _maxAngle = 180;

  uint16_t _minPulseUs = 544;
  uint16_t _maxPulseUs = 2400;

  uint8_t _currentAngle = 90;
  uint8_t _targetAngle = 90;

  float _maxDegPerSec = 360.0f; // default: fairly fast, still smooth
  uint32_t _lastUpdateMs = 0;
};