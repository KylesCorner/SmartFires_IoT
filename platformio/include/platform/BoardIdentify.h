#pragma once

#include <stdint.h>
#include <stddef.h>

struct BoardUniqueId {
  uint32_t w0 = 0;
  uint32_t w1 = 0;
  uint32_t w2 = 0;
  uint32_t w3 = 0;
};

class BoardIdentity {
public:
  static BoardUniqueId read();

  static uint32_t hash32();

  static uint8_t smallId(uint8_t minId, uint8_t maxId);

  static void formatHex(char *out, size_t outSize);
};
