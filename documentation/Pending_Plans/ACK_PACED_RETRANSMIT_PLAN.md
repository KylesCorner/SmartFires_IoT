# ACK-Paced Retransmit Plan (APP_ACK_SUMMARY)

## Purpose

Reduce over-the-air (OTA) bandwidth consumed by duplicate telemetry retransmissions while preserving the current reliability mode (`APP_ACK_SUMMARY`).

This plan introduces ACK-paced retransmission gating on the node so retries occur only after a realistic waiting window, instead of aggressive early retries.

## Scope

In scope:

- Node-side reliability behavior in `TdmaRadioService`.
- TDMA queue and pending-window sizing for delayed retry strategy.
- Runtime defaults for retry timing and pending retention.
- Instrumentation and validation for bandwidth/reliability tradeoff.
- Documentation updates for operations and debugging.

Out of scope:

- Replacing `APP_ACK_SUMMARY` with `STRICT_LINK_ACK` as production default.
- Redesigning Jetson ACK summary protocol payload format.
- RF PHY changes (spreading factor, coding rate, TX power).

## Background

Current `APP_ACK_SUMMARY` behavior keeps fresh telemetry non-blocking and performs app-layer reliability with a pending window.

Observed issue:

- Node retransmits some telemetry packets before a realistic ACK summary window elapses.
- Base receives duplicate packets (same telemetry packet sequence), increasing OTA load.

Root cause pattern:

- Retransmit eligibility is currently governed by retry gap/age, not strict ACK pacing.
- If ACK summary cadence is slower than retry gate timing, retries happen early.

## Objectives

1. Keep `APP_ACK_SUMMARY` mode enabled.
2. Delay retransmission until a realistic ACK window has passed.
3. Maintain or improve end-to-end delivery reliability.
4. Reduce duplicate OTA packet transmissions and base duplicate reception.
5. Preserve fresh-telemetry priority over retransmits.

## Success Criteria

Primary:

1. Duplicate telemetry receptions at base decrease by at least 40 percent in comparable runs.
2. No statistically significant increase in missing packet ratio.
3. No sustained increase in queue overflow (`drop_oldest`) events.

Secondary:

1. Pending-window saturation remains below 80 percent under nominal load.
2. No regression in command/control responsiveness (`CMD_ACK` path).

## Current Technical Constraints

- `TdmaTxQueue` max depth currently hard-limited to 8.
- Reliability pending window hard-limited to 8 entries.
- Typical current defaults:
  - `queueDepth = 4`
  - `reliabilityWindowDepth = 4`
  - `reliabilityMinRetryGapMs = 2000`
  - `reliabilityFreshTrafficHoldoffMs = 2000`
  - `reliabilityMaxAgeMs = 15000`
  - `reliabilityMaxAttempts = 3`

Implication:

- If retransmit is delayed to align with ACK windows, buffer capacities should be expanded to prevent premature drops.

## Design Overview

### Core Change: ACK-Paced Retry Gate

In `APP_ACK_SUMMARY`, a pending telemetry entry is eligible for retransmit only when either condition is true:

1. ACK-summary window condition:
- At least one ACK summary cycle relevant to that entry has had a chance to arrive.

2. Fallback timeout condition:
- Entry age exceeds a bounded fallback wait time to prevent deadlock if ACK summaries stall.

This preserves robustness while suppressing premature retries.

### Eligibility Model

For each pending entry, define:

- `entry_age_ms = now - firstSentMs`
- `retry_wait_ms = clamp(expected_ack_interval_ms * retry_wait_multiplier, retry_wait_min_ms, retry_wait_max_ms)`

Retransmit allowed when:

- `entry_age_ms >= retry_wait_ms` and
- standard gates also pass (`reliabilityMinRetryGapMs`, attempts/age caps, queue priority rules).

Optional stronger gate (feature-flagged):

- require at least one ACK summary observed since entry creation before first retry, unless fallback timeout reached.

## Proposed Config Additions

Add to `TdmaConfig`:

1. `uint32_t expectedAckIntervalMs`
2. `uint16_t retryWaitMultiplierPermille`
3. `uint32_t retryWaitMinMs`
4. `uint32_t retryWaitMaxMs`
5. `bool requireAckSummaryBeforeFirstRetry`

Rationale:

- Avoid floating-point on MCU using permille multiplier.
- Keep wait behavior tunable without code logic changes.

Example defaults (initial candidate):

- `expectedAckIntervalMs = 2000`
- `retryWaitMultiplierPermille = 2000` (2.0x)
- `retryWaitMinMs = 2500`
- `retryWaitMaxMs = 8000`
- `requireAckSummaryBeforeFirstRetry = true`

Derived:

- `retry_wait_ms = clamp(2000 * 2.0, 2500, 8000) = 4000 ms`

## Proposed Queue/Window Sizing

Phase 1 defaults:

1. `queueDepth = 8`
2. `reliabilityWindowDepth = 8`
3. `reliabilityMaxAgeMs = 30000`
4. keep `reliabilityMaxAttempts = 3` initially

Rationale:

- Longer wait-before-retry requires additional buffering headroom.
- Larger pending window allows ACK-paced retries without dropping fresh telemetry history too early.

Memory note:

- Validate SRAM margin after increasing queue/window depth.
- If margin tight, evaluate asymmetric strategy: queue 6, window 8.

## Implementation Plan

### Phase 0: Baseline Capture

1. Run current firmware in controlled test window.
2. Capture:
- node debug log
- base debug log
- receiver log + packet loss summary
3. Compute baseline metrics:
- duplicate ratio at base
- packet loss/missing ratio
- queue/pending drop events

Deliverable:

- baseline metrics table for A/B comparison.

### Phase 1: Config and Data Model

1. Extend `TdmaConfig` with ACK pacing fields.
2. Ensure defaults applied in node config builder paths.
3. Print effective values at boot logs (`tdma`/`radio` streams).

Deliverable:

- build succeeds, logs include new retry-gate params.

### Phase 2: Retry Gate Logic in `TdmaRadioService`

1. Add helper to compute effective `retry_wait_ms`.
2. Add ACK-seen tracking state usable for first-retry gating.
3. Update retransmit candidate selection:
- enforce ACK-paced wait gate before candidate accepted.
4. Keep existing safety gates:
- min retry gap
- max attempts
- max age

Deliverable:

- retransmit path compiles and runs with deterministic gating.

### Phase 3: Buffer Expansion

1. Increase runtime defaults:
- queue depth to 8
- reliability window to 8
2. Validate no regressions in queue behavior (`drop_oldest` semantics unchanged).

Deliverable:

- no increased drop rate under nominal load.

### Phase 4: Observability and Diagnostics

Add logs:

1. `retx_blocked reason=awaiting_ack_window seq=... age_ms=... wait_ms=...`
2. `retx_blocked reason=awaiting_first_ack_summary seq=...`
3. `retx_gate_open reason=ack_window_elapsed|fallback_timeout seq=...`
4. Existing logs retained:
- `retx_candidate`
- `retx_sent`
- `retx_mark_sent`
- `ack_summary_acked`
- `ack_summary_needs_retx`

Deliverable:

- operators can identify why retries are suppressed or allowed.

### Phase 5: Validation Matrix

Run matrix:

1. ACK interval 2.0 s, 1 node
2. ACK interval 4.0 s, 1 node
3. ACK interval 2.0 s, 2+ nodes
4. Loss-injected run (RF attenuation/interference), 2+ nodes

For each run capture:

- duplicate packet counts
- missing/loss counts
- queue/pending depth peaks
- retry counts and timing

Deliverable:

- A/B report with pass/fail against success criteria.

### Phase 6: Rollout and Guardrails

1. Deploy to debug node profile first.
2. Promote to production node profile after passing matrix.
3. Keep emergency rollback switch:
- set `requireAckSummaryBeforeFirstRetry = false`
- revert wait multiplier/min/max to legacy-equivalent behavior.

Deliverable:

- documented rollback recipe and known-good configuration snapshot.

## Code Touchpoints

Expected files:

1. `platformio/include/radio/TdmaConfig.h`
2. `platformio/src/radio/TdmaRadioService.cpp`
3. `platformio/include/radio/TdmaRadioService.h` (if state/fields added)
4. `platformio/src/main.cpp` (node config default values/logs)
5. `platformio/src/main_node_dummy.cpp` (parity for test profile)
6. `documentation/Current_Architecture/PACKET_RELIABILITY.md`
7. `documentation/User_Reference/DEBUG_FILTER.md` (new monitoring grep patterns)

## Validation Metrics Definition

Use these definitions consistently:

1. Duplicate ratio
- `duplicates / total_received_telemetry`

2. Missing ratio
- `missing / (received + missing)`

3. Retry amplification
- `retx_sent / fresh_tx_sent`

4. Queue pressure
- peak `q=count/capacity`
- count of `drop_oldest`

5. Pending pressure
- peak `pendingCount/windowDepth`
- count of `drop_pending` events

## Test Command Set (Reference)

Node monitor:

```bash
SFDBG_SRC=boot,tdma,radio,packet SFDBG_MIN_LEVEL=D SFDBG_SHOW_RAW=0 pio device monitor -e feather_m0_lora_node_debug | tee /tmp/sf-node-debug.log
```

Base monitor:

```bash
SFDBG_SRC=base,app,radio,tdma,packet SFDBG_MIN_LEVEL=D SFDBG_SHOW_RAW=0 pio device monitor -e feather_m0_lora_base | tee /tmp/sf-base-debug.log
```

Edge receiver:

```bash
smartfires-edge receive --port /dev/ttyTHS1 --baud 115200 --data-dir /tmp/sf-ack-paced --ack-interval 2.0 --metrics-interval 5 --raw-log | tee /tmp/sf-receiver.log
```

Quick log extraction:

```bash
rg -n "retx_blocked|retx_candidate|retx_sent|ack_summary_acked|ack_summary_needs_retx|drop_pending|drop_oldest" /tmp/sf-node-debug.log
```

## Risks and Mitigations

1. Risk: delayed retries increase loss under sparse ACK summaries.
- Mitigation: fallback timeout gate and bounded max wait.

2. Risk: larger buffers increase SRAM usage.
- Mitigation: measure memory headroom in debug build; tune queue/window asymmetrically if needed.

3. Risk: over-conservative gating under heavy contention.
- Mitigation: tuning knobs for wait multiplier and min/max bounds.

4. Risk: hidden regressions in command/control latency.
- Mitigation: include CMD/CMD_ACK checks in validation matrix.

## Open Decisions

1. Should first retry require explicit ACK summary observed, or only elapsed wait window?
2. What is canonical expected ACK interval in production (2 s vs 4 s)?
3. Should queue depth and window depth both be 8 in production, or staged (6/8)?

## Acceptance Checklist

1. ACK-paced gates implemented and configurable.
2. Node logs clearly show retry blocked/open reasons.
3. Duplicate ratio reduced at least 40 percent in lab matrix.
4. Missing ratio non-regressing within agreed tolerance.
5. No sustained queue overflow increase.
6. Docs updated and rollback instructions included.

## Rollback Plan

If regressions are observed:

1. Disable strict first-retry ACK dependency (`requireAckSummaryBeforeFirstRetry = false`).
2. Reduce wait policy toward legacy values:
- lower multiplier
- lower min wait
3. If needed, revert to previous config constants while retaining instrumentation.
4. Keep `APP_ACK_SUMMARY` mode active unless directed otherwise.

## Ownership and Execution

Suggested execution sequence:

1. Firmware: config + retry gate + logs.
2. Validation: A/B runs and metrics analysis.
3. Documentation: finalize architecture + ops guidance.
4. Release: staged deployment with rollback guardrails.
