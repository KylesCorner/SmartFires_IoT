#include <unity.h>

#include "radio/TxPowerController.h"

// TxPowerController is deliberately clock-free — every entry point takes an
// explicit nowMs — so these tests drive time directly instead of using
// FakeClock. That is the whole reason the decision loop was factored out of
// SmartFiresBaseApp: the control logic is exercisable without a radio, a clock,
// or an Arduino.

namespace {

constexpr uint8_t kNode = 2;
constexpr int8_t kBaseline = NetworkConfig::kMaxTxPowerDbm;

// SNR (whole dB) that lands the margin comfortably above the step-down
// threshold: floor is -7.5 dB, target margin 10 dB, dead band 3 dB, so
// anything above -7.5 + 13.0 = +5.5 dB triggers a step down.
constexpr int8_t kRoomySnr = 12;
// Margin under target -> jump to baseline. -7.5 + 10.0 = +2.5 dB.
constexpr int8_t kWeakSnr = 0;
// Inside the dead band: above target, below target+deadband.
constexpr int8_t kDeadBandSnr = 4;

// Feeds enough margin samples that an average is well-defined, then delivers a
// STATUS at `nowMs`. Link stats default to unchanged (no retries).
TxPowerController::Decision statusAt(TxPowerController &ctl, uint32_t nowMs,
                                     int8_t snr, int8_t currentDbm,
                                     uint16_t retx = 0, uint16_t fail = 0,
                                     bool isStatic = false) {
  for (int i = 0; i < 8; ++i) {
    ctl.onPacketReceived(kNode, snr, nowMs);
  }
  return ctl.onStatus(kNode, retx, fail, currentDbm, isStatic, nowMs);
}

// Establishes a node with link-stat history so later deltas are meaningful,
// and burns the first decision so the rate limiter is armed.
void primeNode(TxPowerController &ctl, uint32_t nowMs, int8_t currentDbm) {
  statusAt(ctl, nowMs, kDeadBandSnr, currentDbm);
}

}  // namespace

// --- step down --------------------------------------------------------------

void test_roomy_margin_steps_down_one_step() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  const TxPowerController::Decision d =
      statusAt(ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kRoomySnr, kBaseline);

  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
  TEST_ASSERT_EQUAL(TxPowerController::Reason::HeadroomStepDown, d.reason);
  TEST_ASSERT_EQUAL_INT8(kBaseline - BaseConfig::kTxPowerStepDbm, d.targetDbm);
}

void test_step_down_stops_at_floor() {
  TxPowerController ctl;
  primeNode(ctl, 0, NetworkConfig::kMinTxPowerDbm);

  const TxPowerController::Decision d = statusAt(
      ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kRoomySnr,
      NetworkConfig::kMinTxPowerDbm);

  // Already at the floor: clamping makes the target equal to current, so the
  // controller must issue nothing rather than a no-op command.
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);
}

void test_dead_band_holds_steady() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  const TxPowerController::Decision d = statusAt(
      ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kDeadBandSnr, kBaseline);

  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);
}

// --- step up ----------------------------------------------------------------

void test_low_margin_jumps_straight_to_baseline() {
  TxPowerController ctl;
  primeNode(ctl, 0, NetworkConfig::kMinTxPowerDbm);

  const TxPowerController::Decision d = statusAt(
      ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kWeakSnr,
      NetworkConfig::kMinTxPowerDbm);

  // Recovery is one jump, not a walk: the target is the baseline itself, not
  // min + one step.
  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
  TEST_ASSERT_EQUAL(TxPowerController::Reason::MarginLow, d.reason);
  TEST_ASSERT_EQUAL_INT8(kBaseline, d.targetDbm);
}

void test_low_margin_wins_over_retry_inhibitor() {
  TxPowerController ctl;
  primeNode(ctl, 0, NetworkConfig::kMinTxPowerDbm);

  // Retries AND poor margin — the case most in need of power. The inhibitor
  // must not suppress the recovery.
  const TxPowerController::Decision d = statusAt(
      ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kWeakSnr,
      NetworkConfig::kMinTxPowerDbm, /*retx=*/40, /*fail=*/5);

  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
  TEST_ASSERT_EQUAL_INT8(kBaseline, d.targetDbm);
}

// --- retry inhibitor --------------------------------------------------------

void test_retries_inhibit_step_down() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  // Margin is roomy enough to step down, but the interval saw retransmissions.
  // Retries are trusted as evidence of "not comfortable" and nothing more, so
  // they block the optimisation without triggering a power increase.
  const TxPowerController::Decision d =
      statusAt(ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kRoomySnr,
               kBaseline, /*retx=*/3, /*fail=*/0);

  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);
}

void test_retry_delta_spans_decision_interval_not_status_gap() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  // Two STATUS packets inside one decision interval, retx climbing across
  // them. The second is still rate-limited, so no decision comes out...
  TxPowerController::Decision d = statusAt(ctl, 1000, kRoomySnr, kBaseline, 5, 0);
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);
  d = statusAt(ctl, 2000, kRoomySnr, kBaseline, 9, 0);
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);

  // ...and when the interval finally elapses, the delta is measured against
  // the last *decision* (retx=0), not the last STATUS (retx=9). So the retries
  // that happened mid-interval are still seen and still inhibit the step down.
  d = statusAt(ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kRoomySnr,
               kBaseline, 9, 0);
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);
}

// --- pacing -----------------------------------------------------------------

void test_decisions_are_rate_limited_independent_of_status_rate() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  // A build sending STATUS every second (feather_m0_lora_node_debug) must not
  // step power every second.
  for (uint32_t t = 1000; t < BaseConfig::kTxPowerMinDecisionIntervalMs; t += 1000) {
    const TxPowerController::Decision d = statusAt(ctl, t, kRoomySnr, kBaseline);
    TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);
  }

  const TxPowerController::Decision d = statusAt(
      ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kRoomySnr, kBaseline);
  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
}

void test_no_margin_samples_means_no_decision() {
  TxPowerController ctl;
  // STATUS with no preceding frames — nothing to average.
  const TxPowerController::Decision d =
      ctl.onStatus(kNode, 0, 0, kBaseline, false, 0);
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);
}

// --- in-flight gate ---------------------------------------------------------

void test_pending_command_blocks_further_decisions() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  uint32_t t = BaseConfig::kTxPowerMinDecisionIntervalMs;
  TxPowerController::Decision d = statusAt(ctl, t, kRoomySnr, kBaseline);
  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
  ctl.onCommandSent(kNode, d.targetDbm, t);

  t += BaseConfig::kTxPowerMinDecisionIntervalMs;
  d = statusAt(ctl, t, kRoomySnr, kBaseline);
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);
}

void test_failed_enqueue_does_not_arm_the_gate() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  uint32_t t = BaseConfig::kTxPowerMinDecisionIntervalMs;
  TxPowerController::Decision d = statusAt(ctl, t, kRoomySnr, kBaseline);
  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);

  // Command queue was full. The decision must re-arm next interval rather than
  // stalling for a whole ack timeout on a command that never went out.
  ctl.onCommandFailed(kNode);

  t += BaseConfig::kTxPowerMinDecisionIntervalMs;
  d = statusAt(ctl, t, kRoomySnr, kBaseline);
  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
}

void test_unacked_command_expires_and_rearms() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  uint32_t t = BaseConfig::kTxPowerMinDecisionIntervalMs;
  TxPowerController::Decision d = statusAt(ctl, t, kRoomySnr, kBaseline);
  ctl.onCommandSent(kNode, d.targetDbm, t);

  // Just short of the timeout: still gated.
  ctl.update(t + BaseConfig::kCmdAckTimeoutMs - 1);
  d = statusAt(ctl, t + BaseConfig::kCmdAckTimeoutMs - 1, kRoomySnr, kBaseline);
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);

  // Past it: the gate clears and decisions resume.
  t += BaseConfig::kCmdAckTimeoutMs + BaseConfig::kTxPowerMinDecisionIntervalMs;
  ctl.update(t);
  d = statusAt(ctl, t, kRoomySnr, kBaseline);
  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
}

// --- static mode ------------------------------------------------------------

void test_static_mode_suppresses_all_decisions() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);
  ctl.setMode(kNode, BinaryPacket::TX_POWER_MODE_STATIC);

  const TxPowerController::Decision d =
      statusAt(ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kRoomySnr,
               kBaseline, 0, 0, /*isStatic=*/true);

  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);
  TEST_ASSERT_TRUE(ctl.isStatic(kNode));
}

void test_status_reported_mode_overrides_base_belief() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);
  ctl.setMode(kNode, BinaryPacket::TX_POWER_MODE_STATIC);

  // The node reports DYNAMIC — e.g. it rebooted and forgot the pin. The node
  // is authoritative about itself, so the base must follow it back to DYNAMIC
  // rather than leaving the two silently disagreeing.
  const TxPowerController::Decision d =
      statusAt(ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kRoomySnr,
               kBaseline, 0, 0, /*isStatic=*/false);

  TEST_ASSERT_FALSE(ctl.isStatic(kNode));
  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
}

void test_status_reported_power_overrides_base_belief() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  // Node reports a level the base never commanded (a clamp, or a reboot the
  // base missed). Because commands are absolute, the base just adopts it.
  const TxPowerController::Decision d =
      statusAt(ctl, BaseConfig::kTxPowerMinDecisionIntervalMs, kRoomySnr,
               /*currentDbm=*/9);

  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
  TEST_ASSERT_EQUAL_INT8(9 - BaseConfig::kTxPowerStepDbm, d.targetDbm);
}

// --- AWAKEN reset -----------------------------------------------------------

void test_awaken_resets_to_baseline_and_dynamic() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);
  ctl.setMode(kNode, BinaryPacket::TX_POWER_MODE_STATIC);

  ctl.onAwaken(kNode, 5000);

  TEST_ASSERT_FALSE(ctl.isStatic(kNode));
  TEST_ASSERT_EQUAL_INT8(NetworkConfig::kRadioTxPowerDbm, ctl.currentDbm(kNode));
}

void test_awaken_clears_link_stat_baseline() {
  TxPowerController ctl;
  // Node has been running a while with high lifetime counters.
  statusAt(ctl, 0, kRoomySnr, kBaseline, /*retx=*/9000, /*fail=*/400);

  // It reboots — counters restart at zero on the node.
  ctl.onAwaken(kNode, 1000);

  // Without clearing the baseline, 0 - 9000 would wrap to a huge positive
  // delta and inhibit step-down forever. After the reset the first STATUS just
  // re-establishes the baseline, and the next interval can decide normally.
  statusAt(ctl, 2000, kRoomySnr, kBaseline, 0, 0);
  const TxPowerController::Decision d =
      statusAt(ctl, 2000 + BaseConfig::kTxPowerMinDecisionIntervalMs, kRoomySnr,
               kBaseline, 0, 0);

  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
  TEST_ASSERT_EQUAL(TxPowerController::Reason::HeadroomStepDown, d.reason);
}

// --- silence probing --------------------------------------------------------

void test_silence_probes_node_back_to_baseline() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  // Node is stepped down, then goes dark on the uplink only — it never trips
  // its own stale-sync revert because its downlink still works, so the base is
  // the only actor that can restore it.
  uint32_t t = BaseConfig::kTxPowerMinDecisionIntervalMs;
  TxPowerController::Decision d = statusAt(ctl, t, kRoomySnr, kBaseline);
  ctl.onCommandSent(kNode, d.targetDbm, t);
  ctl.onCmdAck(kNode, t);

  t += BaseConfig::kTxPowerSilenceTimeoutMs + 1;
  d = ctl.update(t);

  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
  TEST_ASSERT_EQUAL(TxPowerController::Reason::SilenceProbe, d.reason);
  TEST_ASSERT_EQUAL_INT8(kBaseline, d.targetDbm);
}

void test_silence_probes_are_bounded() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  uint32_t t = BaseConfig::kTxPowerMinDecisionIntervalMs;
  TxPowerController::Decision d = statusAt(ctl, t, kRoomySnr, kBaseline);
  ctl.onCommandSent(kNode, d.targetDbm, t);
  ctl.onCmdAck(kNode, t);

  t += BaseConfig::kTxPowerSilenceTimeoutMs + 1;
  for (uint8_t i = 0; i < BaseConfig::kMaxTxPowerSilenceProbes; ++i) {
    d = ctl.update(t);
    TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, d.action);
    ctl.onCommandFailed(kNode);  // never acked — node really is gone
    t += 1000;
  }

  // Budget spent: the base stops burning slot-0 airtime on a dark node.
  d = ctl.update(t);
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, d.action);
}

void test_hearing_from_node_refills_probe_budget() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  uint32_t t = BaseConfig::kTxPowerMinDecisionIntervalMs;
  TxPowerController::Decision d = statusAt(ctl, t, kRoomySnr, kBaseline);
  ctl.onCommandSent(kNode, d.targetDbm, t);
  ctl.onCmdAck(kNode, t);

  t += BaseConfig::kTxPowerSilenceTimeoutMs + 1;
  for (uint8_t i = 0; i < BaseConfig::kMaxTxPowerSilenceProbes; ++i) {
    ctl.update(t);
    ctl.onCommandFailed(kNode);
    t += 1000;
  }
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, ctl.update(t).action);

  // Node comes back. The budget is for consecutive silences, so it refills.
  ctl.onPacketReceived(kNode, kRoomySnr, t);
  t += BaseConfig::kTxPowerSilenceTimeoutMs + 1;

  TEST_ASSERT_EQUAL(TxPowerController::Action::SetPower, ctl.update(t).action);
}

void test_node_already_at_baseline_is_not_probed() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  // Silent, but already at baseline — a probe could not change anything, so
  // spending slot-0 airtime on one is pure waste.
  const uint32_t t = BaseConfig::kTxPowerSilenceTimeoutMs + 1;
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, ctl.update(t).action);
}

void test_static_node_is_never_probed() {
  TxPowerController ctl;
  primeNode(ctl, 0, kBaseline);

  uint32_t t = BaseConfig::kTxPowerMinDecisionIntervalMs;
  TxPowerController::Decision d = statusAt(ctl, t, kRoomySnr, kBaseline);
  ctl.onCommandSent(kNode, d.targetDbm, t);
  ctl.onCmdAck(kNode, t);
  ctl.setMode(kNode, BinaryPacket::TX_POWER_MODE_STATIC);

  t += BaseConfig::kTxPowerSilenceTimeoutMs + 1;
  TEST_ASSERT_EQUAL(TxPowerController::Action::None, ctl.update(t).action);
}

// --- oscillation guard ------------------------------------------------------

void test_step_down_does_not_immediately_retrigger_step_up() {
  // The dead band exists so a node sitting near target cannot step down, land
  // under target, and be jumped straight back up forever. Assert the invariant
  // the static_assert in BaseConfig.h guarantees, from the loop's side.
  TEST_ASSERT_GREATER_THAN_INT16(
      static_cast<int16_t>(BaseConfig::kTxPowerStepDbm) * 10,
      BaseConfig::kSnrDeadBandDbX10);
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_roomy_margin_steps_down_one_step);
  RUN_TEST(test_step_down_stops_at_floor);
  RUN_TEST(test_dead_band_holds_steady);

  RUN_TEST(test_low_margin_jumps_straight_to_baseline);
  RUN_TEST(test_low_margin_wins_over_retry_inhibitor);

  RUN_TEST(test_retries_inhibit_step_down);
  RUN_TEST(test_retry_delta_spans_decision_interval_not_status_gap);

  RUN_TEST(test_decisions_are_rate_limited_independent_of_status_rate);
  RUN_TEST(test_no_margin_samples_means_no_decision);

  RUN_TEST(test_pending_command_blocks_further_decisions);
  RUN_TEST(test_failed_enqueue_does_not_arm_the_gate);
  RUN_TEST(test_unacked_command_expires_and_rearms);

  RUN_TEST(test_static_mode_suppresses_all_decisions);
  RUN_TEST(test_status_reported_mode_overrides_base_belief);
  RUN_TEST(test_status_reported_power_overrides_base_belief);

  RUN_TEST(test_awaken_resets_to_baseline_and_dynamic);
  RUN_TEST(test_awaken_clears_link_stat_baseline);

  RUN_TEST(test_silence_probes_node_back_to_baseline);
  RUN_TEST(test_silence_probes_are_bounded);
  RUN_TEST(test_hearing_from_node_refills_probe_budget);
  RUN_TEST(test_node_already_at_baseline_is_not_probed);
  RUN_TEST(test_static_node_is_never_probed);

  RUN_TEST(test_step_down_does_not_immediately_retrigger_step_up);

  UNITY_END();
  return 0;
}
