// ---
// description: Reads the SAMD21 128-bit hardware serial number and derives an FNV-1a uid_hash/small-id for board identity.
// role: implementation
// ---
#include "platform/BoardIdentify.h"

#include <stdio.h>

#if defined(ARDUINO_ARCH_SAMD)

#define SAMD21_SERIAL_WORD_0 (*(volatile uint32_t *)0x0080A00C)
#define SAMD21_SERIAL_WORD_1 (*(volatile uint32_t *)0x0080A040)
#define SAMD21_SERIAL_WORD_2 (*(volatile uint32_t *)0x0080A044)
#define SAMD21_SERIAL_WORD_3 (*(volatile uint32_t *)0x0080A048)

#endif

BoardUniqueId BoardIdentity::read() {
  BoardUniqueId id;

#if defined(ARDUINO_ARCH_SAMD)
  id.w0 = SAMD21_SERIAL_WORD_0;
  id.w1 = SAMD21_SERIAL_WORD_1;
  id.w2 = SAMD21_SERIAL_WORD_2;
  id.w3 = SAMD21_SERIAL_WORD_3;
#else
  // Native/unit-test fallback.
  id.w0 = 0x12345678;
  id.w1 = 0x9ABCDEF0;
  id.w2 = 0x13572468;
  id.w3 = 0x24681357;
#endif

  return id;
}

uint32_t BoardIdentity::hash32() {
  const BoardUniqueId id = read();
  const uint32_t words[4] = {id.w0, id.w1, id.w2, id.w3};

  // FNV-1a 32-bit hash.
  uint32_t hash = 2166136261UL;

  for (size_t i = 0; i < 4; ++i) {
    uint32_t word = words[i];

    for (size_t b = 0; b < 4; ++b) {
      const uint8_t byte = static_cast<uint8_t>((word >> (8 * b)) & 0xFF);
      hash ^= byte;
      hash *= 16777619UL;
    }
  }

  return hash;
}

uint8_t BoardIdentity::smallId(uint8_t minId, uint8_t maxId) {
  if (maxId < minId) {
    return minId;
  }

  const uint8_t range = static_cast<uint8_t>(maxId - minId + 1);
  return static_cast<uint8_t>(minId + (hash32() % range));
}

void BoardIdentity::formatHex(char *out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }

  const BoardUniqueId id = read();

  snprintf(out, outSize, "%08lX%08lX%08lX%08lX",
           static_cast<unsigned long>(id.w0),
           static_cast<unsigned long>(id.w1),
           static_cast<unsigned long>(id.w2),
           static_cast<unsigned long>(id.w3));
}
