# Network Parameter Consolidation Plan

## Purpose

Create a single source of truth for all SmartFires network tuning parameters across:

- Node firmware (TDMA, queueing, reliability)
- Base firmware bridge behavior (health and forwarding context)
- Edge receiver runtime (ACK and sync cadence)

The objective is to make network tuning deterministic, auditable, and safe to change without hidden coupling between components.

## Problem Statement

Network tuning inputs are currently distributed across:

- compile-time defines (`platformio.ini`)
- firmware defaults (`TdmaConfig`, node setup logic)
- edge CLI runtime flags (`smartfires-edge receive`)
- architecture docs spread across multiple files

This fragmentation increases risk of:

1. conflicting assumptions (for example ACK cadence vs retry windows)
2. accidental regressions during field tuning
3. difficult root-cause analysis when reliability changes

## Scope

In scope:

1. Inventory all network-related tunables in one plan.
2. Define ownership and change process per parameter group.
3. Define baseline profiles (debug, lab, production).
4. Define compatibility rules between parameters.
5. Define rollout/rollback and validation gates.

Out of scope:

1. protocol redesign of packet formats.
2. hardware changes (antenna, radio module, power chain).
3. replacing AppLayerAckSummary architecture.

## Source Locations (Current)

Primary code/config sources:

1. `platformio/platformio.ini`
2. `platformio/include/radio/TdmaConfig.h`
3. `platformio/include/radio/TdmaTxQueue.h`
4. `platformio/src/main.cpp`
5. `edge/edge-receiver/src/smartfires_edge/main.py`

Supporting architecture docs:

1. `documentation/Current_Architecture/TDMA_PROTOCOL.md`
2. `documentation/Current_Architecture/PACKET_RELIABILITY.md`

## Consolidated Parameter Catalog

## A) Build-Time and Environment Parameters

| Parameter | Layer | Current Default | Current Source | Owner | Change Frequency | Notes |
|---|---|---:|---|---|---|---|
| `NUM_SLOTS` | Node firmware | `4` | `platformio.ini` | Firmware | Low | Must match all nodes in deployment |
| `SMARTFIRES_TDMA_RELIABILITY_MODE` | Node firmware | `1` (`APP_ACK_SUMMARY`) | `platformio.ini` | Firmware | Low | Keep `1` in this plan |
| `SMARTFIRES_STATUS_INTERVAL_MS` | Node app packet generation | `1000` debug / `10000` node env / fallback 15 min | `platformio.ini`, `main.cpp` | Firmware + Ops | Medium | Directly influences offered load |
| `monitor_speed` | Serial monitor | `115200` | `platformio.ini` | Ops | Low | Debug transport only |

## B) TDMA Timing Parameters (Node)

| Parameter | Layer | Current Default | Current Source | Owner | Safe Initial Range |
|---|---|---:|---|---|---|
| `slotWidthMs` | TDMA | `900` | `TdmaConfig` | Firmware | 800 to 1200 |
| `guardMs` | TDMA | `20` | `TdmaConfig` | Firmware | 10 to 40 |
| `syncStaleMs` | TDMA | `1320000` (22 min) | `TdmaConfig` | Firmware | 600000 to 1800000 |

## C) Queue and Buffering Parameters (Node)

| Parameter | Layer | Current Default | Current Source | Owner | Hard Cap / Range |
|---|---|---:|---|---|---|
| `queueDepth` | TX queue | `4` | `TdmaConfig` and node setup | Firmware | Runtime <= 8 (current hard cap) |
| `TdmaTxQueue::MaxDepth` | TX queue capacity cap | `8` | `TdmaTxQueue.h` | Firmware | Compile-time cap |
| `reliabilityWindowDepth` | Pending reliability window | `4` | `TdmaConfig` | Firmware | Runtime <= 8 |
| `kMaxReliabilityWindow` | Pending window cap | `8` | `TdmaRadioService.h` | Firmware | Compile-time cap |
| `MaxPayloadLen` | Payload buffer capacity | `220` | `TdmaConfig` | Firmware | Must stay >= max packet size |

## D) Link-ACK Path Parameters (Node)

| Parameter | Layer | Current Default | Current Source | Owner | Safe Initial Range |
|---|---|---:|---|---|---|
| `enableLinkAck` | Link-layer behavior | computed by mode | `main.cpp` | Firmware | mode dependent |
| `maxRetries` | Link ACK retries | `3` (node setup override) | `main.cpp` | Firmware | 0 to 5 |
| `ackTimeoutMs` | Link ACK timeout | `250` (node setup override) | `main.cpp` | Firmware | 80 to 400 |

## E) App Reliability Parameters (Node)

| Parameter | Layer | Current Default | Current Source | Owner | Safe Initial Range |
|---|---|---:|---|---|---|
| `enableAppReliability` | App reliability | `true` | `TdmaConfig` | Firmware | true for this plan |
| `reliabilityMaxAttempts` | Pending retry limit | `3` | `TdmaConfig` | Firmware | 2 to 5 |
| `reliabilityMaxAgeMs` | Pending age limit | `15000` | `TdmaConfig` | Firmware | 10000 to 45000 |
| `reliabilityMinRetryGapMs` | Minimum retry spacing | `2000` | `TdmaConfig` | Firmware | 1500 to 8000 |
| `reliabilityFreshTrafficHoldoffMs` | Holdoff after fresh send | `2000` | `TdmaConfig` | Firmware | 1000 to 8000 |

## F) Edge Runtime Parameters

| Parameter | Layer | Current Default | Current Source | Owner | Safe Initial Range |
|---|---|---:|---|---|---|
| `--ack-interval` | ACK summary cadence | `4.0` s | edge CLI parser | Edge/Ops | 1.0 to 6.0 |
| `--sync-interval` | TIME_SYNC cadence | `600` s | edge CLI parser | Edge/Ops | 120 to 900 |
| `--metrics-interval` | Metrics persistence | `10` s | edge CLI parser | Edge/Ops | 5 to 30 |
| `--nodes` | tracked node IDs | `[1, 2]` | edge CLI parser | Edge/Ops | deployment dependent |
| `--raw-log` | frame logging toggle | off by default | edge CLI parser | Ops | debug only |

## Compatibility Rules (Must Hold)

1. `NUM_SLOTS` must be identical across all deployed nodes.
2. If `SMARTFIRES_TDMA_RELIABILITY_MODE=1`, edge `--ack-interval` must be set and monitored.
3. Retry pacing should not be shorter than practical ACK cadence horizon.
4. If `queueDepth`/`reliabilityWindowDepth` are increased, verify memory headroom before deployment.
5. `SMARTFIRES_STATUS_INTERVAL_MS` and `NUM_SLOTS` must be tuned together to avoid chronic queue pressure.

## Baseline Parameter Profiles

### Profile A: Debug (high visibility)

- `NUM_SLOTS=4`
- `SMARTFIRES_TDMA_RELIABILITY_MODE=1`
- `SMARTFIRES_STATUS_INTERVAL_MS=1000`
- `queueDepth=4` (or 8 when testing ACK-paced retry)
- edge `--ack-interval=2.0`
- edge `--sync-interval=600`
- verbose node/base logging enabled

### Profile B: Lab Stress

- `NUM_SLOTS=4` or 6 (test-specific)
- `SMARTFIRES_TDMA_RELIABILITY_MODE=1`
- `SMARTFIRES_STATUS_INTERVAL_MS=1000 to 3000`
- `queueDepth=8`
- `reliabilityWindowDepth=8`
- edge `--ack-interval=1.0 to 2.0`
- controlled RF attenuation/interference

### Profile C: Production Candidate

- `NUM_SLOTS=4` (unless scaling decision changes)
- `SMARTFIRES_TDMA_RELIABILITY_MODE=1`
- `SMARTFIRES_STATUS_INTERVAL_MS=10000` (or approved value)
- queue/window values from validated test matrix
- edge `--ack-interval` fixed and documented in deployment playbook

## Governance and Ownership Model

## Change Classes

Class 1 (Low risk):

- edge runtime only (`--ack-interval`, `--sync-interval`, logging toggles)

Class 2 (Medium risk):

- firmware runtime defaults (`TdmaConfig` values)
- build flags affecting load (`SMARTFIRES_STATUS_INTERVAL_MS`)

Class 3 (High risk):

- compile-time caps (`MaxDepth`, `kMaxReliabilityWindow`)
- slot geometry changes (`slotWidthMs`, `guardMs`, `NUM_SLOTS`)

## Approval Path

1. Class 1: single maintainer approval + validation run.
2. Class 2: firmware + edge owner approval + A/B metrics.
3. Class 3: formal review with rollback plan and staged rollout.

## Required Change Record Template

For every parameter change, record:

1. parameter name
2. old value and new value
3. reason for change
4. expected impact
5. validation runs and metrics
6. rollback trigger and rollback value

## Validation and Observability Plan

## Mandatory Metrics

1. Duplicate ratio at base/edge
2. Missing ratio
3. Retry amplification (`retx_sent / fresh_tx_sent`)
4. Queue pressure (`drop_oldest`, peak queue occupancy)
5. Pending pressure (`drop_pending`, peak pending occupancy)
6. ACK health (summary cadence consistency)

## Required Logs

Node monitor (`radio`, `tdma`, `packet`) must include:

- `tx_sent`
- `retx_candidate`
- `retx_sent`
- `ack_summary_acked`
- `ack_summary_needs_retx`
- `drop_oldest`
- `drop_pending`

Base monitor (`base`) must include:

- `health_link`
- `health_rx`
- `rx_lora`
- `tx_ack_summary`

Edge receiver logs must include:

- `ACK_TX` cadence indicators
- packet loss summary output

## Rollout Strategy

1. Establish baseline metrics with unchanged parameters.
2. Apply one parameter group at a time (no mixed large changes).
3. Validate against baseline with equivalent runtime duration.
4. Promote from debug -> lab stress -> production candidate.

## Rollback Criteria

Rollback if any condition persists:

1. duplicate ratio worsens versus baseline by >10 percent
2. missing ratio worsens beyond agreed tolerance
3. queue/pending drops rise above baseline for sustained windows
4. command/control responsiveness degrades

## Immediate Consolidation Deliverables

1. Maintain this file as the canonical parameter index for network tuning.
2. Link this file from `documentation/README.md` under architecture/operations references.
3. Keep active profile values in a dedicated appendix or companion file once finalized.

## Implementation Work Plan

### Phase 1: Inventory Freeze

1. Confirm current values in code and runtime scripts.
2. Record profile snapshots (debug/lab/production candidate).

### Phase 2: Parameter Registry

1. Add a machine-readable parameter table (optional JSON or YAML) in docs.
2. Add script/checklist for verifying mismatches before test runs.

### Phase 3: Tuning Workflow Standardization

1. Define standard experiment template.
2. Define mandatory metrics collection commands.
3. Define decision thresholds for promotion/rollback.

### Phase 4: Continuous Maintenance

1. Update this file on every approved parameter change.
2. Add date and commit reference for each updated profile.

## Open Decisions

1. Target production `--ack-interval` (2.0 s vs 4.0 s).
2. Whether to keep `SMARTFIRES_STATUS_INTERVAL_MS` split between debug and production envs or align test/prod closer.
3. Whether to raise compile-time queue/window hard caps beyond 8 after memory review.

## Acceptance Criteria for This Plan

1. All major TDMA and reliability tunables are listed in one place.
2. Owners and approval paths are explicit.
3. Compatibility rules and rollback triggers are documented.
4. This file is adopted as the single reference before tuning changes.
