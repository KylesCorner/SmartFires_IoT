#include <unity.h>

#include "platform/Samd21RtcTicks.h"

using Samd21RtcTicks::msToTicks;
using Samd21RtcTicks::tickDelta;
using Samd21RtcTicks::ticksToMs;

void test_zero_ms_is_zero_ticks() {
  TEST_ASSERT_EQUAL_UINT32(0, msToTicks(0));
  TEST_ASSERT_EQUAL_UINT32(0, ticksToMs(0));
}

void test_known_conversions() {
  // 1000 ms is exactly 1024 ticks — the calibration point.
  TEST_ASSERT_EQUAL_UINT32(1024, msToTicks(1000));
  TEST_ASSERT_EQUAL_UINT32(1000, ticksToMs(1024));

  // 1 ms must round *up* to a whole tick, never down to zero.
  TEST_ASSERT_EQUAL_UINT32(2, msToTicks(1));

  // One tick floors to 0 ms (0.977 ms truncated).
  TEST_ASSERT_EQUAL_UINT32(0, ticksToMs(1));
}

void test_alarm_never_early() {
  // Ceiling conversion: the tick count must always cover at least the
  // requested duration, so a MODE0 compare alarm can't fire early.
  for (uint32_t ms = 1; ms <= 5000; ++ms) {
    const uint64_t tickSpanUs =
        static_cast<uint64_t>(msToTicks(ms)) * 1000000ULL /
        Samd21RtcTicks::kTicksPerSecond;
    TEST_ASSERT_TRUE(tickSpanUs >= static_cast<uint64_t>(ms) * 1000ULL);
  }
}

void test_round_trip_error_bounded() {
  // Round trip is exact (ceil then floor) — assert the plan's <= 1 ms
  // bound and the stronger exactness across the 1 ms..1 h duty range.
  const uint32_t samples[] = {1,      2,      10,     250,     999,
                              1000,   1001,   1500,   30000,   300000,
                              600000, 900000, 1800000, 3600000};
  for (uint32_t ms : samples) {
    TEST_ASSERT_EQUAL_UINT32(ms, ticksToMs(msToTicks(ms)));
  }
  for (uint32_t ms = 1; ms <= 10000; ++ms) {
    TEST_ASSERT_EQUAL_UINT32(ms, ticksToMs(msToTicks(ms)));
  }
}

void test_tick_delta_simple() {
  TEST_ASSERT_EQUAL_UINT32(0, tickDelta(500, 500));
  TEST_ASSERT_EQUAL_UINT32(1024, tickDelta(1000, 2024));
}

void test_tick_delta_wraparound() {
  // Counter wraps at 2^32 ticks (~48.5 days at 1024 Hz). Delta must
  // stay correct across the wrap.
  TEST_ASSERT_EQUAL_UINT32(11, tickDelta(0xFFFFFFFAUL, 5));
  TEST_ASSERT_EQUAL_UINT32(1, tickDelta(0xFFFFFFFFUL, 0));
  TEST_ASSERT_EQUAL_UINT32(1024, tickDelta(0xFFFFFE00UL, 0x00000200UL));
}

void test_hour_sleep_tick_target() {
  // Worst practical case from the plan: 1 h sleep. 3,600,000 ms at
  // 1024 ticks/s = 3,686,400 ticks — comfortably inside u32.
  TEST_ASSERT_EQUAL_UINT32(3686400UL, msToTicks(3600000UL));
  TEST_ASSERT_EQUAL_UINT32(3600000UL, ticksToMs(3686400UL));
}

int main() {
  delay(2000);
  UNITY_BEGIN();

  RUN_TEST(test_zero_ms_is_zero_ticks);
  RUN_TEST(test_known_conversions);
  RUN_TEST(test_alarm_never_early);
  RUN_TEST(test_round_trip_error_bounded);
  RUN_TEST(test_tick_delta_simple);
  RUN_TEST(test_tick_delta_wraparound);
  RUN_TEST(test_hour_sleep_tick_target);

  UNITY_END();
  return 0;
}
