#include <unity.h>

#include <string.h>

#include "radio/TdmaClock.h"
#include "radio/TdmaConfig.h"
#include "radio/TdmaRadioService.h"
#include "radio/TdmaTxQueue.h"
#include "telemetry/BinaryPacket.h"

#include "../support/fakes/FakeClock.h"
#include "../support/fakes/FakeTdmaRadioDriver.h"

// Mirrors NetworkConfig.h's shipped AppLayerAckSummary profile, since the
// behaviour under test is a relationship between kReliabilityMaxAgeMs and the
// Timed duty cycle's standby length — both real numbers, not arbitrary ones.
static constexpr uint32_t kMaxAgeMs = 30000;
static constexpr uint32_t kRetryWaitMs = 8000;  // 4000 ms * 2.0, inside [4500, 10000]
static constexpr uint32_t kStandbyMs = 35000;   // kTimedSleepMs — deliberately > kMaxAgeMs
static constexpr uint32_t kFrameMs = 4 * 900;

static TdmaConfig makeConfig() {
  TdmaConfig cfg;
  cfg.nodeId = 2;  // slot 1 — slot 0 is the base's
  cfg.baseAddr = 1;
  cfg.numSlots = 4;
  cfg.slotWidthMs = 900;
  cfg.guardMs = 20;
  cfg.syncStaleMs = 1320000;
  cfg.rxWakeAheadMs = 50;
  cfg.queueDepth = 8;
  cfg.enableLinkAck = false;
  cfg.enableAppReliability = true;
  cfg.reliabilityWindowDepth = 8;
  cfg.reliabilityMaxAttempts = 3;
  cfg.reliabilityMaxAgeMs = kMaxAgeMs;
  cfg.reliabilityMinRetryGapMs = 2000;
  cfg.reliabilityFreshTrafficHoldoffMs = 2000;
  cfg.reliabilityMode = TdmaReliabilityMode::AppLayerAckSummary;
  cfg.expectedAckIntervalMs = 4000;
  cfg.retryWaitMultiplierPermille = 2000;
  cfg.retryWaitMinMs = 4500;
  cfg.retryWaitMaxMs = 10000;
  cfg.requireAckSummaryBeforeFirstRetry = false;
  return cfg;
}

// The window's last bundle — sent in the final slot before standby, so its ack
// can only arrive in a slot 0 that falls after the radio is already off. That
// makes it the frame this whole mechanism exists to keep alive across the sleep.
static uint8_t makeFinalWindowBundle(uint8_t nodeId, uint8_t seq, uint8_t *buf,
                                     size_t bufSize) {
  BinaryPacket::FullStatePayload ref = {};
  ref.session_time = 1000;
  ref.sensor_flags = 0x02;
  ref.temp_cdegc = 2100;

  return BinaryPacket::encodeBundlePayload(
      nodeId, seq, ref, nullptr, 0, buf, bufSize, /*flags=*/0);
}

// PKT_WINDOW_BEGIN, the frame the node sends on waking. Deliberately built the
// same way the node app builds it: seq 0, its own window_id, no telemetry seq to
// burn.
static uint8_t makeWindowBegin(uint8_t nodeId, uint16_t windowId, uint8_t *buf,
                               size_t bufSize) {
  BinaryPacket::WindowMarkerPayload marker = {};
  marker.session_time_ms = 12000;
  marker.window_id = windowId;

  return BinaryPacket::encodeWindowMarkerPayload(
      BinaryPacket::PKT_WINDOW_BEGIN, nodeId, marker, buf, bufSize);
}

// Walks the fake clock forward to a comfortable position inside this node's own
// slot, so drainTxQueue() both passes myTurn() and has slot budget left.
static void advanceIntoOwnSlot(FakeClock &clock, const TdmaClock &tdma) {
  for (uint32_t i = 0; i < 2 * kFrameMs; ++i) {
    uint32_t slotIndex = 0;
    if (tdma.myTurn(slotIndex) && tdma.positionInSlotMs() >= 100 &&
        tdma.positionInSlotMs() <= 300) {
      return;
    }
    clock.advance(1);
  }
  TEST_FAIL_MESSAGE("never reached this node's slot");
}

struct Rig {
  FakeClock clock;
  TdmaConfig cfg;
  TdmaClock tdma;
  TdmaTxQueue queue;
  FakeTdmaRadioDriver driver;
  TdmaRadioService svc;

  Rig()
      : cfg(makeConfig()), tdma(cfg, clock), queue(8), svc(cfg, tdma, queue, driver) {
    clock.set(10000);
    tdma.applySync(/*sessionId=*/1, /*sessionTimeMs=*/10000);
    svc.begin();
  }

  // Enqueues the window's final bundle and gets it on the air, leaving exactly
  // one sent-but-unacked entry in the pending window.
  void sendFinalWindowBundle() {
    uint8_t buf[BinaryPacket::kMaxBundleLoRaSize] = {};
    const uint8_t len = makeFinalWindowBundle(cfg.nodeId, /*seq=*/7, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(svc.enqueueTelemetry(buf, len));

    advanceIntoOwnSlot(clock, tdma);
    svc.update();

    TEST_ASSERT_EQUAL_UINT8(1, driver.sentCount);
    TEST_ASSERT_EQUAL_UINT8(0, svc.queuedCount());
    driver.clearSent();
  }

  // Enqueues PKT_WINDOW_BEGIN and gets it on the air.
  void sendWindowBegin() {
    uint8_t buf[BinaryPacket::kWindowMarkerLoRaSize] = {};
    const uint8_t len = makeWindowBegin(cfg.nodeId, /*windowId=*/3, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(svc.enqueueTelemetry(buf, len));

    advanceIntoOwnSlot(clock, tdma);
    svc.update();
  }
};

void test_pending_entry_is_lost_across_standby_without_notify() {
  Rig rig;
  rig.sendFinalWindowBundle();

  // Standby with no notifyMcuStandby(): the session clock ran through the sleep
  // (rtc-subsecond-sleep Phase 2), so the entry now looks 35 s old against a
  // 30 s ceiling and is discarded before it can ever be retried.
  rig.clock.advance(kStandbyMs);

  for (uint8_t i = 0; i < 8; ++i) {
    advanceIntoOwnSlot(rig.clock, rig.tdma);
    rig.svc.update();
    rig.clock.advance(kFrameMs);
  }

  TEST_ASSERT_EQUAL_UINT8(0, rig.driver.sentCount);
  TEST_ASSERT_EQUAL_UINT32(0, rig.svc.retransmitCount());
}

void test_notify_mcu_standby_keeps_pending_entry_retransmittable() {
  Rig rig;
  rig.sendFinalWindowBundle();

  rig.clock.advance(kStandbyMs);
  rig.svc.notifyMcuStandby(kStandbyMs);

  // Standby no longer counts against the entry, so it ages normally from here
  // and becomes eligible once the ACK-paced retry wait elapses.
  rig.clock.advance(kRetryWaitMs);
  advanceIntoOwnSlot(rig.clock, rig.tdma);
  rig.svc.update();

  TEST_ASSERT_EQUAL_UINT8(1, rig.driver.sentCount);
  TEST_ASSERT_EQUAL_UINT32(1, rig.svc.retransmitCount());
}

void test_retransmit_is_stamped_retx() {
  Rig rig;
  rig.sendFinalWindowBundle();

  rig.clock.advance(kStandbyMs);
  rig.svc.notifyMcuStandby(kStandbyMs);
  rig.clock.advance(kRetryWaitMs);
  advanceIntoOwnSlot(rig.clock, rig.tdma);
  rig.svc.update();

  TEST_ASSERT_EQUAL_UINT8(1, rig.driver.sentCount);
  const FakeTdmaRadioDriver::SentFrame *frame = rig.driver.last();
  TEST_ASSERT_NOT_NULL(frame);
  if (frame == nullptr) {
    return;
  }

  BinaryPacket::PktHeader hdr = {};
  memcpy(&hdr, frame->data, sizeof(hdr));

  // RETX tells the base its last ACK_SUMMARY never landed. Nothing else is
  // stamped: the sleep/wake signal rides its own PKT_WINDOW_END/BEGIN frames, so
  // there is no longer a window bit whose meaning inverts on a replay.
  TEST_ASSERT_TRUE((hdr.flags & BinaryPacket::PKT_FLAG_RETX) != 0);
  TEST_ASSERT_EQUAL_UINT8(BinaryPacket::PKT_FLAG_RETX, hdr.flags);
  TEST_ASSERT_EQUAL_UINT8(7, hdr.seq);

  // The stamp rewrites a header byte, so the trailing crc8 must have been
  // recomputed or every receiver would reject the frame outright.
  TEST_ASSERT_EQUAL_UINT8(
      BinaryPacket::crc8(frame->data, frame->len - 1),
      frame->data[frame->len - 1]);
}

void test_repeated_retransmits_are_byte_identical() {
  Rig rig;
  rig.sendFinalWindowBundle();

  rig.clock.advance(kStandbyMs);
  rig.svc.notifyMcuStandby(kStandbyMs);
  rig.clock.advance(kRetryWaitMs);
  advanceIntoOwnSlot(rig.clock, rig.tdma);
  rig.svc.update();
  TEST_ASSERT_EQUAL_UINT8(1, rig.driver.sentCount);

  // A new slot index and past reliabilityMinRetryGapMs, so a second attempt is
  // allowed. If the stamp had been applied to the stored pending payload rather
  // than the outgoing copy, this frame's crc8 would cover a doubly-stamped
  // header and differ from the first.
  rig.clock.advance(kFrameMs);
  advanceIntoOwnSlot(rig.clock, rig.tdma);
  rig.svc.update();
  TEST_ASSERT_EQUAL_UINT8(2, rig.driver.sentCount);

  TEST_ASSERT_EQUAL_UINT8(rig.driver.sent[0].len, rig.driver.sent[1].len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(rig.driver.sent[0].data, rig.driver.sent[1].data,
                                rig.driver.sent[0].len);
}

// A marker must never take a pending slot: it is fire-and-forget, carries no
// telemetry seq, and an entry for it could never be acked — it would just age
// out and burn retransmissions on a frame nobody tracks.
void test_window_marker_does_not_enter_the_pending_window() {
  Rig rig;
  rig.sendWindowBegin();

  TEST_ASSERT_EQUAL_UINT8(1, rig.driver.sentCount);

  // Nothing pending, so a full retry wait later there is nothing to resend.
  rig.clock.advance(kRetryWaitMs * 2);
  advanceIntoOwnSlot(rig.clock, rig.tdma);
  rig.svc.update();

  TEST_ASSERT_EQUAL_UINT8(1, rig.driver.sentCount);
  TEST_ASSERT_EQUAL_UINT32(0, rig.svc.retransmitCount());
}

// The point of the whole design: on waking, the node says "I'm back" with a
// 17-byte marker and then waits for the ack the base deferred, instead of
// retransmitting a full bundle just to prompt it.
void test_window_begin_holds_off_a_due_retransmit() {
  Rig rig;
  rig.sendFinalWindowBundle();

  rig.clock.advance(kStandbyMs);
  rig.svc.notifyMcuStandby(kStandbyMs);

  // Retry gate is now open — without the hold this bundle would go straight back
  // on the air in the next slot.
  rig.clock.advance(kRetryWaitMs);

  rig.sendWindowBegin();
  const uint8_t sentAfterBegin = rig.driver.sentCount;

  // One frame period on: still inside the ack round trip the hold covers.
  rig.clock.advance(kFrameMs);
  advanceIntoOwnSlot(rig.clock, rig.tdma);
  rig.svc.update();
  TEST_ASSERT_EQUAL_UINT8(sentAfterBegin, rig.driver.sentCount);
  TEST_ASSERT_EQUAL_UINT32(0, rig.svc.retransmitCount());

  // Past the hold with still no ack, the retransmission does fire — the hold
  // delays the retry, it must not cancel it, or a lost WINDOW_BEGIN would strand
  // the bundle permanently.
  rig.clock.advance(kFrameMs * 2 + kRetryWaitMs);
  advanceIntoOwnSlot(rig.clock, rig.tdma);
  rig.svc.update();
  TEST_ASSERT_EQUAL_UINT32(1, rig.svc.retransmitCount());
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_pending_entry_is_lost_across_standby_without_notify);
  RUN_TEST(test_notify_mcu_standby_keeps_pending_entry_retransmittable);
  RUN_TEST(test_retransmit_is_stamped_retx);
  RUN_TEST(test_window_marker_does_not_enter_the_pending_window);
  RUN_TEST(test_window_begin_holds_off_a_due_retransmit);
  RUN_TEST(test_repeated_retransmits_are_byte_identical);

  UNITY_END();
  return 0;
}
