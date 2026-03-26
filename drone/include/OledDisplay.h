#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#include "IActuator.h"
#include "II2CDevice.h"

class OledDisplay : public IActuator, public II2CDevice {
public:
  enum class Controller : uint8_t {
    SH1106,
    SSD1306
  };

  explicit OledDisplay(
      Controller controller = Controller::SH1106,
      uint8_t i2cAddress = 0x3C,
      const char* displayName = "OLED 128x64")
      : _controller(controller),
        _address(i2cAddress),
        _name(displayName) {}

  // ---- IActuator ----
  const char* name() const override { return _name; }

  bool begin() override {
    return begin(Wire);
  }

  void update() override {
    if (!_healthy || !_begun || !_dirty) {
      return;
    }

    drawFrame();
    _dirty = false;
  }

  bool healthy() const override {
    return _healthy;
  }

  // ---- II2CDevice ----
  uint8_t address() const override {
    return _address;
  }

  bool begin(TwoWire& wire = Wire) override {
    _wire = &wire;

    _wire->begin();
    _wire->setClock(400000);

    if (!ping()) {
      _healthy = false;
      _begun = false;
      return false;
    }

    _u8g2 = createDisplay();

    if (_u8g2 == nullptr) {
      _healthy = false;
      _begun = false;
      return false;
    }

    _u8g2->begin();
    _u8g2->setI2CAddress(static_cast<uint8_t>(_address << 1)); // U8g2 expects 8-bit form
    _u8g2->clearBuffer();
    _u8g2->setFont(u8g2_font_6x10_tf);
    _u8g2->drawStr(0, 10, "OLED init OK");
    _u8g2->sendBuffer();

    _healthy = true;
    _begun = true;
    _dirty = false;
    return true;
  }

  bool ping() override {
    if (_wire == nullptr) return false;
    _wire->beginTransmission(_address);
    return (_wire->endTransmission() == 0);
  }

  // ---- High-level display API ----
  void clear() {
    _line1[0] = '\0';
    _line2[0] = '\0';
    _line3[0] = '\0';
    _line4[0] = '\0';
    _dirty = true;
  }

  void printLine(uint8_t line, const char* text) {
    if (text == nullptr) text = "";

    switch (line) {
      case 0: copyLine(_line1, sizeof(_line1), text); break;
      case 1: copyLine(_line2, sizeof(_line2), text); break;
      case 2: copyLine(_line3, sizeof(_line3), text); break;
      case 3: copyLine(_line4, sizeof(_line4), text); break;
      default: return;
    }

    _dirty = true;
  }

  void printfLine(uint8_t line, const char* fmt, ...) {
    char buffer[32];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    printLine(line, buffer);
  }

private:
  TwoWire* _wire = nullptr;
  U8G2* _u8g2 = nullptr;

  Controller _controller;
  uint8_t _address;
  const char* _name;

  bool _healthy = false;
  bool _begun = false;
  bool _dirty = false;

  char _line1[32] = "";
  char _line2[32] = "";
  char _line3[32] = "";
  char _line4[32] = "";

  static void copyLine(char* dst, size_t dstSize, const char* src) {
    if (dstSize == 0) return;
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
  }

  U8G2* createDisplay() {
    // Full-buffer constructors, hardware I2C, no reset pin.
    // Use SH1106 first because many 1.3" 128x64 modules are SH1106-class.
    switch (_controller) {
      case Controller::SH1106:
        return new U8G2_SH1106_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE);

      case Controller::SSD1306:
        return new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE);

      default:
        return nullptr;
    }
  }

  void drawFrame() {
    if (_u8g2 == nullptr) return;

    _u8g2->clearBuffer();
    _u8g2->setFont(u8g2_font_6x10_tf);

    _u8g2->drawStr(0, 12, _line1);
    _u8g2->drawStr(0, 27, _line2);
    _u8g2->drawStr(0, 42, _line3);
    _u8g2->drawStr(0, 57, _line4);

    _u8g2->sendBuffer();
  }
};