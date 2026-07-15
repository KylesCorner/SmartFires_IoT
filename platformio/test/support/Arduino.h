#pragma once

#include <stdint.h>
#include <stddef.h>

// Minimal no-op stand-ins for the Arduino Print API so headers like
// logging/DebugLogger.h compile natively. Output is discarded.
class Print {
public:
  template <typename T>
  void print(T) {}
  template <typename T>
  void println(T) {}
  void println() {}
};

#ifndef F
#define F(x) (x)
#endif

// Analog pin aliases — values are arbitrary on native.
static const uint8_t A0 = 14;
static const uint8_t A1 = 15;
static const uint8_t A2 = 16;
static const uint8_t A3 = 17;
static const uint8_t A4 = 18;
static const uint8_t A5 = 19;
static const uint8_t A6 = 20;
static const uint8_t A7 = 21;

#define HIGH 0x1
#define LOW  0x0

#define INPUT        0x0
#define OUTPUT       0x1
#define INPUT_PULLUP 0x2

inline void delay(uint32_t ms) {
  (void)ms;
}

inline uint32_t millis() {
  return 0;
}

inline void pinMode(uint8_t pin, uint8_t mode) {
  (void)pin;
  (void)mode;
}

inline void digitalWrite(uint8_t pin, uint8_t value) {
  (void)pin;
  (void)value;
}

inline int digitalRead(uint8_t pin) {
  (void)pin;
  return LOW;
}

inline int analogRead(uint8_t pin) {
  (void)pin;
  return 0;
}

class FakeSerialForNative {
public:
  void begin(unsigned long baud) {
    (void)baud;
  }

  void print(const char *s) {
    (void)s;
  }

  void print(char c) {
    (void)c;
  }

  void print(int v) {
    (void)v;
  }

  void print(unsigned int v) {
    (void)v;
  }

  void print(long v) {
    (void)v;
  }

  void print(unsigned long v) {
    (void)v;
  }

  void print(float v) {
    (void)v;
  }

  void println() {}

  void println(const char *s) {
    (void)s;
  }

  void println(char c) {
    (void)c;
  }

  void println(int v) {
    (void)v;
  }

  void println(unsigned int v) {
    (void)v;
  }

  void println(long v) {
    (void)v;
  }

  void println(unsigned long v) {
    (void)v;
  }

  void println(float v) {
    (void)v;
  }
};

inline FakeSerialForNative Serial;
