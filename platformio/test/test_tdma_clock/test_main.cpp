#include <unity.h>

#include "radio/TdmaClock.h"

#include "../support/fakes/FakeClock.h"

static TdmaConfig makeTestConfig() {
  // Mirrors NetworkConfig.h's production geometry (kSlotWidthMs/kGuardMs/
  // kSyncStaleMs/kNumSlots default) so test timings map directly onto the
  // real frame layout described in tdma-protocol.
  TdmaConfig cfg;
  cfg.nodeId = 2;  // slot 1 — deliberately not slot 0, the base's slot
  cfg.numSlots = 4;
  cfg.slotWidthMs = 900;
  cfg.guardMs = 20;
  cfg.syncStaleMs = 1320000;
  cfg.rxWakeAheadMs = 50;
  return cfg;
}

void test_pre_sync_rx_window_always_open() {
  FakeClock clock;
  TdmaClock tdma(makeTestConfig(), clock);

  // No applySync() call yet — hasSync() is false. baseRxWindowOpen() must
  // stay true unconditionally, same as myTurn()'s pre-sync fallback, since
  // slot timing can't be trusted until the first TIME_SYNC arrives.
  TEST_ASSERT_FALSE(tdma.hasSync());
  TEST_ASSERT_TRUE(tdma.baseRxWindowOpen());

  clock.advance(5000);
  TEST_ASSERT_TRUE(tdma.baseRxWindowOpen());
}

void test_synced_rx_window_open_during_base_slot() {
  FakeClock clock;
  TdmaClock tdma(makeTestConfig(), clock);

  clock.set(1000);
  tdma.applySync(/*sessionId=*/1, /*sessionTimeMs=*/0);

  // sessionNow == 0 -> slot 0 (the base's slot), full 900 ms width, no
  // guard-band exclusion (unlike myTurn(), which carves guard off both ends).
  TEST_ASSERT_EQUAL_UINT8(0, tdma.currentSlotNumber());
  TEST_ASSERT_TRUE(tdma.baseRxWindowOpen());

  clock.advance(899);  // sessionNow == 899, still slot 0
  TEST_ASSERT_EQUAL_UINT8(0, tdma.currentSlotNumber());
  TEST_ASSERT_TRUE(tdma.baseRxWindowOpen());
}

void test_synced_rx_window_closed_outside_base_slot() {
  FakeClock clock;
  TdmaClock tdma(makeTestConfig(), clock);

  clock.set(1000);
  tdma.applySync(/*sessionId=*/1, /*sessionTimeMs=*/0);

  clock.advance(900);  // sessionNow == 900 -> slot 1 (my own slot)
  TEST_ASSERT_EQUAL_UINT8(1, tdma.currentSlotNumber());
  TEST_ASSERT_FALSE(tdma.baseRxWindowOpen());

  clock.advance(900);  // sessionNow == 1800 -> slot 2 (a sibling node's slot)
  TEST_ASSERT_EQUAL_UINT8(2, tdma.currentSlotNumber());
  TEST_ASSERT_FALSE(tdma.baseRxWindowOpen());

  clock.advance(900);  // sessionNow == 2700 -> slot 3
  TEST_ASSERT_EQUAL_UINT8(3, tdma.currentSlotNumber());
  TEST_ASSERT_FALSE(tdma.baseRxWindowOpen());

  clock.advance(900);  // sessionNow == 3600 -> wraps to slot 0 again
  TEST_ASSERT_EQUAL_UINT8(0, tdma.currentSlotNumber());
  TEST_ASSERT_TRUE(tdma.baseRxWindowOpen());
}

void test_rx_window_wake_ahead_opens_before_base_slot() {
  FakeClock clock;
  TdmaClock tdma(makeTestConfig(), clock);

  clock.set(0);
  tdma.applySync(/*sessionId=*/1, /*sessionTimeMs=*/0);

  // Slot 3 is the last slot in the frame (numSlots=4), immediately preceding
  // slot 0's wraparound. sessionNow == 2700 -> start of slot 3, well before
  // the 50 ms wake-ahead window (which starts at slotWidthMs - rxWakeAheadMs
  // = 850 ms into the slot, i.e. sessionNow == 3550).
  clock.advance(2700);
  TEST_ASSERT_EQUAL_UINT8(3, tdma.currentSlotNumber());
  TEST_ASSERT_FALSE(tdma.baseRxWindowOpen());

  clock.advance(849);  // sessionNow == 3549, 1 ms before the wake-ahead window
  TEST_ASSERT_EQUAL_UINT8(3, tdma.currentSlotNumber());
  TEST_ASSERT_FALSE(tdma.baseRxWindowOpen());

  clock.advance(1);  // sessionNow == 3550 -> wake-ahead window opens
  TEST_ASSERT_EQUAL_UINT8(3, tdma.currentSlotNumber());
  TEST_ASSERT_TRUE(tdma.baseRxWindowOpen());

  clock.advance(49);  // sessionNow == 3599, last ms of slot 3
  TEST_ASSERT_EQUAL_UINT8(3, tdma.currentSlotNumber());
  TEST_ASSERT_TRUE(tdma.baseRxWindowOpen());

  clock.advance(1);  // sessionNow == 3600 -> slot 0 itself, still open
  TEST_ASSERT_EQUAL_UINT8(0, tdma.currentSlotNumber());
  TEST_ASSERT_TRUE(tdma.baseRxWindowOpen());
}

void test_rx_window_independent_of_own_slot_assignment() {
  // baseRxWindowOpen() must not depend on mySlot()/nodeId at all -- every
  // node, regardless of which slot it owns, listens during slot 0 because
  // that's the only slot the base ever transmits in.
  FakeClock clockA;
  TdmaConfig cfgA = makeTestConfig();
  cfgA.nodeId = 2;  // slot 1
  TdmaClock tdmaA(cfgA, clockA);

  FakeClock clockB;
  TdmaConfig cfgB = makeTestConfig();
  cfgB.nodeId = 4;  // slot 3
  TdmaClock tdmaB(cfgB, clockB);

  clockA.set(0);
  clockB.set(0);
  tdmaA.applySync(1, 0);
  tdmaB.applySync(1, 0);

  TEST_ASSERT_EQUAL(tdmaA.baseRxWindowOpen(), tdmaB.baseRxWindowOpen());
  TEST_ASSERT_TRUE(tdmaA.baseRxWindowOpen());

  clockA.advance(900);
  clockB.advance(900);
  TEST_ASSERT_EQUAL(tdmaA.baseRxWindowOpen(), tdmaB.baseRxWindowOpen());
  TEST_ASSERT_FALSE(tdmaA.baseRxWindowOpen());
}

void test_stale_sync_falls_back_to_always_open() {
  FakeClock clock;
  TdmaConfig cfg = makeTestConfig();
  cfg.syncStaleMs = 1000;
  TdmaClock tdma(cfg, clock);

  clock.set(0);
  tdma.applySync(/*sessionId=*/1, /*sessionTimeMs=*/0);

  clock.advance(900);  // slot 1, fresh sync -> closed
  TEST_ASSERT_FALSE(tdma.syncStale());
  TEST_ASSERT_FALSE(tdma.baseRxWindowOpen());

  clock.advance(200);  // now 1100 ms since sync -> stale (> syncStaleMs)
  TEST_ASSERT_TRUE(tdma.syncStale());
  TEST_ASSERT_TRUE(tdma.baseRxWindowOpen());
}

int main() {
  delay(2000);
  UNITY_BEGIN();

  RUN_TEST(test_pre_sync_rx_window_always_open);
  RUN_TEST(test_synced_rx_window_open_during_base_slot);
  RUN_TEST(test_synced_rx_window_closed_outside_base_slot);
  RUN_TEST(test_rx_window_wake_ahead_opens_before_base_slot);
  RUN_TEST(test_rx_window_independent_of_own_slot_assignment);
  RUN_TEST(test_stale_sync_falls_back_to_always_open);

  UNITY_END();
  return 0;
}
