---
name: phase-progress
description: Tracking doc for the staged rollout of TDMA telemetry reliability changes, from StrictLinkAck mode introduction through fire-and-forget app-layer ACK_SUMMARY.
category: plan-completed
status: historical
superseded_by: packet-reliability
---

# SmartFires Reliability Phase Progress

This file tracks the staged rollout of TDMA telemetry reliability changes.

## Current Status

- Phase 1: complete
- Phase 2: complete in code, ready for ongoing runtime validation
- Phase 3: started in code
- Phase 4: started in code
- Phase 5: started in code

## Phase 1

Goal:
Introduce an explicit telemetry reliability mode without changing deployed behavior.

Delivered:

- `TdmaReliabilityMode` added to `TdmaConfig`
- `StrictLinkAck` and `AppLayerAckSummary` modes defined
- telemetry send path routed through mode selection instead of a hardcoded link-ACK decision
- real and dummy node builds kept on strict per-packet link ACK during this phase

Validation:

- runtime-tested by user after Phase 1 commit
- node and base builds completed in VS Code task workflow

## Phase 2

Goal:
Move node telemetry to fire-and-forget uplink with app-layer `ACK_SUMMARY` reliability, while preserving the existing control-plane behavior.

Delivered:

- node and dummy-node environments now build with `SMARTFIRES_TDMA_RELIABILITY_MODE=1`
- telemetry packets in that mode do not use per-packet link ACK
- `AWAKEN`, direct `TIME_SYNC`, and base `ACK_SUMMARY` control behavior remain on the current direct reliable path
- runtime logs now print the effective telemetry reliability mode
- strict mode remains available through `SMARTFIRES_TDMA_RELIABILITY_MODE=0`

Validation steps:

1. Build `feather_m0_lora_node`
2. Build `feather_m0_lora_base`
3. Flash both targets
4. Confirm node boot log shows `TELEM_REL_MODE: APP_ACK_SUMMARY`
5. Confirm telemetry still arrives at the base
6. Confirm `ACK_SUMMARY` packets are still received and clear pending telemetry
7. Confirm sampling cadence remains stable when radio acknowledgments are delayed or absent

Known note:

- shell validation with `pio` was not available in this chat terminal because `pio` / `platformio` were not on PATH, so validation here relied on editor diagnostics plus the user's VS Code build workflow.

## Phase 3

Goal:
Tune retransmit policy using the existing pending window so new telemetry and retransmits coexist cleanly under app-layer reliability.

Delivered so far:

- added a short holdoff after each fresh telemetry send before idle retransmits are eligible again
- kept queue-first behavior intact so fresh telemetry still wins whenever new payloads are available
- added base-side `[BaseApp][SEQ20]` receipt summaries to make end-to-end sequence coverage easier to judge during runtime testing
- changed pending-window replacement to evict the stalest retransmit candidate first, using retry attempts and age instead of raw oldest-first replacement

Remaining work:

- prefer fresh telemetry over aggressive retransmit bursts
- enforce bounded retransmit age and attempt limits
- validate queue and pending-window behavior under induced packet loss

## Phase 4

Goal:
Coalesce base-side `ACK_SUMMARY` emission so summaries are sent from current ack state rather than immediately on every qualifying packet.

Delivered so far:

- telemetry receive now marks per-node ACK state dirty instead of sending `ACK_SUMMARY` inline for each packet
- base update loop flushes at most one current `ACK_SUMMARY` per dirty node each cycle
- redundant summaries are skipped when the latest cumulative ACK state is unchanged from the last sent summary
- base ACK-summary flush is now globally paced and round-robin across dirty nodes, so summary downlink is bounded instead of tracking the raw loop rate

Remaining work:

- tune pacing against observed node/base traffic if the current interval is still too chatty or too slow
- keep link-layer reliability on `ACK_SUMMARY`

## Phase 5

Goal:
Evaluate moving `ACK_SUMMARY` emission into a stricter base-station TDMA downlink window if needed.

Delivered so far:

- added a minimum base-side interval between ACK-summary sends
- changed the base flush path to send at most one ACK summary per paced interval
- rotated the starting tracker index so dirty nodes are served fairly instead of always favoring lower node IDs
- ACK-summary flushing is now constrained to the base station TDMA slot window using the shared session-time schedule

Remaining work:

- verify downlink budget for multiple nodes
- ensure one lost summary is harmless because the next summary supersedes it
- avoid introducing ack-of-ack behavior

## Reliability Mode Reference

- `SMARTFIRES_TDMA_RELIABILITY_MODE=0`
  `StrictLinkAck` telemetry mode. Each telemetry packet waits for link-layer ACK and uses retry attempts.

- `SMARTFIRES_TDMA_RELIABILITY_MODE=1`
  `AppLayerAckSummary` telemetry mode. Telemetry is sent without per-packet link ACK and is reconciled through periodic `ACK_SUMMARY` packets.
