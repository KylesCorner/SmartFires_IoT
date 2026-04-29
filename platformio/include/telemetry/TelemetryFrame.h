#pragma once

#include <stddef.h>
#include <stdint.h>

struct TelemetryFrame {
  static constexpr size_t MaxLen = 220;

  char payload[MaxLen] = {};
  size_t len = 0;

  void clear() {
    payload[0] = '\0';
    len = 0;
  }
};
