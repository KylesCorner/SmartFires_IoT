#include <unity.h>
#include <cstring>

#include "fakes/FakeClock.h"
#include "fakes/FakeTdmaRadioDriver.h"

#include "radio/TdmaClock.h"
#include "radio/TdmaRadioService.h"
#include "radio/TdmaTxQueue.h"

static void assertState(TdmaRadioService &svc, TdmaRadioState expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(svc.state()));
}

static void assertError(TdmaRadioService &svc, TdmaRadioError expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(svc.error()));
}

static void makePayload(uint8_t *out, uint8_t len, uint8_t seed) {
  for (uint8_t i = 0; i < len; ++i) {
    out[i] = static_cast<uint8_t>(seed + i);
  }
}

void test_tdma_begin_success() {
  FakeClock clock;
  TdmaConfig cfg;
  TdmaClock tdmaClock(cfg, clock);
  TdmaTxQueue queue(cfg.queueDepth);
  FakeTdmaRadioDriver radio;

  TdmaRadioService service(cfg, tdmaClock, queue, radio);

  TEST_ASSERT_TRUE(service.begin());
  TEST_ASSERT_TRUE(radio.beginCalled);
  assertState(service, TdmaRadioState::Ready);
  assertError(service, TdmaRadioError::None);
}

void test_tdma_begin_failure_enters_error() {
  FakeClock clock;
  TdmaConfig cfg;
  TdmaClock tdmaClock(cfg, clock);
  TdmaTxQueue queue(cfg.queueDepth);
  FakeTdmaRadioDriver radio;
  radio.beginOk = false;

  TdmaRadioService service(cfg, tdmaClock, queue, radio);

  TEST_ASSERT_FALSE(service.begin());
  assertState(service, TdmaRadioState::Error);
  assertError(service, TdmaRadioError::BeginFailed);
}

void test_no_sync_transmits_immediately_fallback_mode() {
  FakeClock clock;
  TdmaConfig cfg;
  cfg.nodeId = 1;
  cfg.baseAddr = 0x01;
  cfg.slotWidthMs = 900;
  cfg.numSlots = 2;

  TdmaClock tdmaClock(cfg, clock);
  TdmaTxQueue queue(cfg.queueDepth);
  FakeTdmaRadioDriver radio;

  TdmaRadioService service(cfg, tdmaClock, queue, radio);

  uint8_t payload[5];
  makePayload(payload, sizeof(payload), 10);

  TEST_ASSERT_TRUE(service.begin());
  TEST_ASSERT_TRUE(service.enqueueTelemetry(payload, sizeof(payload)));

  service.update();

  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);
  TEST_ASSERT_EQUAL_UINT8(cfg.baseAddr, radio.lastTo);
  TEST_ASSERT_EQUAL_UINT8(sizeof(payload), radio.lastSentLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, radio.lastSent, sizeof(payload));
  TEST_ASSERT_EQUAL_UINT32(1, service.sentCount());
}

void test_synced_node_only_transmits_in_its_slot_not_guard() {
  FakeClock clock;
  TdmaConfig cfg;
  cfg.nodeId = 2;       // slot 1
  cfg.numSlots = 2;
  cfg.slotWidthMs = 900;
  cfg.guardMs = 20;

  TdmaClock tdmaClock(cfg, clock);
  TdmaTxQueue queue(cfg.queueDepth);
  FakeTdmaRadioDriver radio;

  TdmaRadioService service(cfg, tdmaClock, queue, radio);

  uint8_t payload[3] = {1, 2, 3};

  TEST_ASSERT_TRUE(service.begin());

  // Sync session to 0 at local time 0.
  tdmaClock.applySync(0);

  TEST_ASSERT_TRUE(service.enqueueTelemetry(payload, sizeof(payload)));

  // Slot 0, node 2 should not transmit.
  clock.set(100);
  service.update();
  TEST_ASSERT_EQUAL_UINT32(0, radio.sendCount);

  // Slot 1 leading guard: session 900 + 10.
  clock.set(910);
  service.update();
  TEST_ASSERT_EQUAL_UINT32(0, radio.sendCount);

  // Slot 1 inside TX window.
  clock.set(925);
  service.update();
  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);
}

void test_trailing_guard_blocks_transmission() {
  FakeClock clock;
  TdmaConfig cfg;
  cfg.nodeId = 1; // slot 0
  cfg.numSlots = 2;
  cfg.slotWidthMs = 900;
  cfg.guardMs = 20;

  TdmaClock tdmaClock(cfg, clock);
  TdmaTxQueue queue(cfg.queueDepth);
  FakeTdmaRadioDriver radio;

  TdmaRadioService service(cfg, tdmaClock, queue, radio);

  uint8_t payload[3] = {1, 2, 3};

  TEST_ASSERT_TRUE(service.begin());
  tdmaClock.applySync(0);

  TEST_ASSERT_TRUE(service.enqueueTelemetry(payload, sizeof(payload)));

  clock.set(881); // trailing guard starts at 880.
  service.update();

  TEST_ASSERT_EQUAL_UINT32(0, radio.sendCount);
}

void test_only_one_tx_per_slot() {
  FakeClock clock;
  TdmaConfig cfg;
  cfg.nodeId = 1;
  cfg.numSlots = 2;
  cfg.slotWidthMs = 900;
  cfg.guardMs = 20;

  TdmaClock tdmaClock(cfg, clock);
  TdmaTxQueue queue(cfg.queueDepth);
  FakeTdmaRadioDriver radio;

  TdmaRadioService service(cfg, tdmaClock, queue, radio);

  uint8_t p1[2] = {1, 1};
  uint8_t p2[2] = {2, 2};

  TEST_ASSERT_TRUE(service.begin());
  tdmaClock.applySync(0);

  TEST_ASSERT_TRUE(service.enqueueTelemetry(p1, sizeof(p1)));
  TEST_ASSERT_TRUE(service.enqueueTelemetry(p2, sizeof(p2)));

  clock.set(25);
  service.update();
  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);
  TEST_ASSERT_EQUAL_UINT8(1, service.queuedCount());

  clock.set(100);
  service.update();
  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);
  TEST_ASSERT_EQUAL_UINT8(1, service.queuedCount());

  // Next frame, node 1 slot again.
  clock.set(1800 + 25);
  service.update();
  TEST_ASSERT_EQUAL_UINT32(2, radio.sendCount);
  TEST_ASSERT_EQUAL_UINT8(0, service.queuedCount());
}

void test_queue_drops_oldest_when_full() {
  TdmaTxQueue queue(2);

  uint8_t p1[1] = {1};
  uint8_t p2[1] = {2};
  uint8_t p3[1] = {3};

  TEST_ASSERT_TRUE(queue.enqueue(p1, 1));
  TEST_ASSERT_TRUE(queue.enqueue(p2, 1));
  TEST_ASSERT_TRUE(queue.enqueue(p3, 1));

  TEST_ASSERT_EQUAL_UINT8(2, queue.count());
  TEST_ASSERT_EQUAL_UINT32(1, queue.droppedOldestCount());

  uint8_t out[10];
  uint8_t len = 0;

  TEST_ASSERT_TRUE(queue.dequeue(out, len));
  TEST_ASSERT_EQUAL_UINT8(1, len);
  TEST_ASSERT_EQUAL_UINT8(2, out[0]);

  TEST_ASSERT_TRUE(queue.dequeue(out, len));
  TEST_ASSERT_EQUAL_UINT8(1, len);
  TEST_ASSERT_EQUAL_UINT8(3, out[0]);
}

void test_sync_packet_updates_tdma_clock() {
  FakeClock clock;
  TdmaConfig cfg;
  cfg.nodeId = 2;
  cfg.numSlots = 2;
  cfg.slotWidthMs = 900;
  cfg.guardMs = 20;

  TdmaClock tdmaClock(cfg, clock);
  TdmaTxQueue queue(cfg.queueDepth);
  FakeTdmaRadioDriver radio;

  TdmaRadioService service(cfg, tdmaClock, queue, radio);

  TEST_ASSERT_TRUE(service.begin());

  radio.queueRxString("TS,900");
  service.update();

  TEST_ASSERT_TRUE(tdmaClock.hasSync());
  TEST_ASSERT_EQUAL_UINT32(900, tdmaClock.sessionNowMs());
  TEST_ASSERT_EQUAL_UINT8(1, tdmaClock.currentSlotNumber());
}

void test_stale_sync_falls_back_to_immediate_tx() {
  FakeClock clock;
  TdmaConfig cfg;
  cfg.nodeId = 2;
  cfg.numSlots = 2;
  cfg.slotWidthMs = 900;
  cfg.guardMs = 20;
  cfg.syncStaleMs = 1000;

  TdmaClock tdmaClock(cfg, clock);
  TdmaTxQueue queue(cfg.queueDepth);
  FakeTdmaRadioDriver radio;

  TdmaRadioService service(cfg, tdmaClock, queue, radio);

  uint8_t payload[2] = {9, 9};

  TEST_ASSERT_TRUE(service.begin());

  tdmaClock.applySync(0);

  clock.set(1501); // sync is stale
  TEST_ASSERT_TRUE(tdmaClock.syncStale());

  TEST_ASSERT_TRUE(service.enqueueTelemetry(payload, sizeof(payload)));
  service.update();

  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);
}

void test_send_failure_is_counted_and_packet_is_dropped() {
  FakeClock clock;
  TdmaConfig cfg;

  TdmaClock tdmaClock(cfg, clock);
  TdmaTxQueue queue(cfg.queueDepth);
  FakeTdmaRadioDriver radio;
  radio.sendOk = false;

  TdmaRadioService service(cfg, tdmaClock, queue, radio);

  uint8_t payload[3] = {7, 8, 9};

  TEST_ASSERT_TRUE(service.begin());
  TEST_ASSERT_TRUE(service.enqueueTelemetry(payload, sizeof(payload)));

  service.update();

  TEST_ASSERT_EQUAL_UINT32(0, service.sentCount());
  TEST_ASSERT_EQUAL_UINT32(1, service.failedSendCount());
  TEST_ASSERT_EQUAL_UINT8(0, service.queuedCount());
  assertError(service, TdmaRadioError::SendFailed);
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_tdma_begin_success);
  RUN_TEST(test_tdma_begin_failure_enters_error);
  RUN_TEST(test_no_sync_transmits_immediately_fallback_mode);
  RUN_TEST(test_synced_node_only_transmits_in_its_slot_not_guard);
  RUN_TEST(test_trailing_guard_blocks_transmission);
  RUN_TEST(test_only_one_tx_per_slot);
  RUN_TEST(test_queue_drops_oldest_when_full);
  RUN_TEST(test_sync_packet_updates_tdma_clock);
  RUN_TEST(test_stale_sync_falls_back_to_immediate_tx);
  RUN_TEST(test_send_failure_is_counted_and_packet_is_dropped);

  return UNITY_END();
}
