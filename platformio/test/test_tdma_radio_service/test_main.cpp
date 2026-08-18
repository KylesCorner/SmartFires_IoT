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

// One bundle carrying the window-close marker — the frame whose ack can only
// ever arrive in a slot 0 that falls after standby has already begun, and so the
// exact frame this whole mechanism exists to keep alive.
static uint8_t makeWindowLastBundle(uint8_t nodeId, uint8_t seq, uint8_t *buf,
                                    size_t bufSize) {
  BinaryPacket::FullStatePayload ref = {};
  ref.session_time = 1000;
  ref.sensor_flags = 0x02;
  ref.temp_cdegc = 2100;

  return BinaryPacket::encodeBundlePayload(
      nodeId, seq, ref, nullptr, 0, buf, bufSize,
      BinaryPacket::PKT_FLAG_WINDOW_LAST);
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

  // Enqueues the window-close bundle and gets it on the air, leaving exactly one
  // sent-but-unacked entry in the pending window.
  void sendWindowLastBundle() {
    uint8_t buf[BinaryPacket::kMaxBundleLoRaSize] = {};
    const uint8_t len = makeWindowLastBundle(cfg.nodeId, /*seq=*/7, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(svc.enqueueTelemetry(buf, len));

    advanceIntoOwnSlot(clock, tdma);
    svc.update();

    TEST_ASSERT_EQUAL_UINT8(1, driver.sentCount);
    TEST_ASSERT_EQUAL_UINT8(0, svc.queuedCount());
    driver.clearSent();
  }
};

void test_pending_entry_is_lost_across_standby_without_notify() {
  Rig rig;
  rig.sendWindowLastBundle();

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
  rig.sendWindowLastBundle();

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

void test_retransmit_is_stamped_retx_and_keeps_window_marker() {
  Rig rig;
  rig.sendWindowLastBundle();

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

  // RETX added, WINDOW_LAST preserved: the base needs both to tell "this node
  // is awake and re-asking" from "this node is about to sleep".
  TEST_ASSERT_TRUE((hdr.flags & BinaryPacket::PKT_FLAG_RETX) != 0);
  TEST_ASSERT_TRUE((hdr.flags & BinaryPacket::PKT_FLAG_WINDOW_LAST) != 0);
  TEST_ASSERT_EQUAL_UINT8(7, hdr.seq);

  // The stamp rewrites a header byte, so the trailing crc8 must have been
  // recomputed or every receiver would reject the frame outright.
  TEST_ASSERT_EQUAL_UINT8(
      BinaryPacket::crc8(frame->data, frame->len - 1),
      frame->data[frame->len - 1]);
}

void test_repeated_retransmits_are_byte_identical() {
  Rig rig;
  rig.sendWindowLastBundle();

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

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_pending_entry_is_lost_across_standby_without_notify);
  RUN_TEST(test_notify_mcu_standby_keeps_pending_entry_retransmittable);
  RUN_TEST(test_retransmit_is_stamped_retx_and_keeps_window_marker);
  RUN_TEST(test_repeated_retransmits_are_byte_identical);

  UNITY_END();
  return 0;
}
