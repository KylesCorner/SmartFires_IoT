---
name: tunable-parameters
description: Every tunable constant in the system — TDMA, sensing/duty-cycle, power, Jetson.
category: architecture
status: current
last_verified: 2026-09-04
source_refs:
  - platformio/include/config/NetworkConfig.h
  - platformio/include/config/SensingConfig.h
  - platformio/include/config/PowerConfig.h
  - platformio/include/config/BaseConfig.h
related_docs:
  - tdma-protocol
  - packet-reliability
  - duty-cycling
---

# Tunable parameters

This is the operating-value index. Firmware defaults live in `platformio/include/config/`; build-time selectors live in `platformio.ini`; Jetson runtime defaults live in `smartfires_edge/config.py`. Change the source, not a copied value in this page, and recheck all derived constraints.

## Build selectors

| Flag | Active value | Meaning |
|---|---:|---|
| `NUM_SLOTS` | 5 | Base plus four node slots; shared by all LoRa environments |
| `SMARTFIRES_TDMA_RELIABILITY_MODE` | 1 on all node environments | `AppLayerAckSummary`; 0 selects StrictLinkAck |
| `SMARTFIRES_STATUS_INTERVAL_MS` | 15,000 on all current node environments | STATUS cadence |
| `SMARTFIRES_DUTY_CYCLE_MODE` | 1 production, 2 debug/timed, 3 hybrid | 0 Continuous, 1 SensorTriggered, 2 Timed, 3 Hybrid |

`NetworkConfig.h` has conservative fallbacks (`NUM_SLOTS=4`, StrictLinkAck, STATUS 15 minutes), but they are not the values compiled by active node targets.

## NetworkConfig

### TDMA and radio

| Constant | Value | Notes |
|---|---:|---|
| `kNumSlots` | 5 | From build flag |
| `kSlotWidthMs` | 900 ms | One slot |
| `kGuardMs` | 20 ms | Applied at both edges; usable span is 860 ms |
| `kFramePeriodMs` | 4,500 ms | Derived |
| `kSyncStaleMs` | 1,320,000 ms | 22 min |
| `kRxWakeAheadMs` | 150 ms | Before base slot 0 |
| `kBaseAddr` | 1 | Base radio address/node ID |
| `kRadioFrequencyMhz` | 915.0 | Raw LoRa carrier |
| `kRadioTxPowerDbm` | 13 dBm | Boot/baseline power |
| `kMinTxPowerDbm` / `kMaxTxPowerDbm` | 5 / 13 dBm | Node clamp for control commands |
| `kLinkRetries` | 3 | RadioHead retries after first attempt |
| `kLinkAckTimeoutMs` | 250 ms | Remote ACK wait |
| `kAckTxWaitMs` | 90 ms | Bound for local ACK TX completion |
| `kSendTxWaitMs` | 340 ms | Bound for local packet TX completion |

### Per-packet TX budgets

| Constant | Value |
|---|---:|
| `kBundleTxBudgetMs` | 340 ms |
| `kStatusTxBudgetMs` | 120 ms |
| `kAwakenTxBudgetMs` | 90 ms |
| `kDefaultTxBudgetMs` | 140 ms |

### Queue and reliability

| Constant | Value |
|---|---:|
| `kQueueDepth` | 8 |
| `kReliabilityWindowDepth` | 8 |
| `kReliabilityMaxAttempts` | 3 total attempts |
| `kReliabilityMaxAgeMs` | 30,000 ms |
| `kReliabilityMinRetryGapMs` | 2,000 ms |
| `kReliabilityFreshTrafficHoldoffMs` | 2,000 ms |
| `kExpectedAckIntervalMs` | frame period = 4,500 ms |
| `kRetryWaitMultiplierPermille` | 2,000 = 2.0x |
| `kRetryWaitMinMs` / `Max` | 4,500 / 10,000 ms |
| Derived current retry wait | 9,000 ms |
| `kRequireAckSummaryBeforeFirstRetry` | false |
| `kAwakenIntervalMs` | 5,000 ms |
| `kEnableTelemetryTx` | true |

The retry floor must cover at least one frame. Increasing `NUM_SLOTS` to 6 without retuning it triggers a static assertion.

## SensingConfig

### Controller profiles

| Profile | Warmup | Sample | Active | Scheduled period | Other |
|---|---:|---:|---:|---:|---|
| Continuous | 10 s | 750 ms | unbounded | none | no intentional sleep |
| SensorTriggered | 10 s | 750 ms | 30 s | none | 3 s minimum sleep; 1 °C / 5 %RH trigger |
| Timed | 10 s | 1,000 ms | 30 s | 75 s | 15 s overrun ceiling; 5 s minimum standby |
| Hybrid | 10 s | 750 ms | 30 s | 340 s | trigger or timer; 5 s minimum standby |

Shared controller bounds: minimum worthwhile MCU standby is 250 ms; maximum final TX drain before Timed standby is 5 seconds. A full bundle is 15 samples. Timed selects two bundles per window, so its active duration derives to 30 seconds.

### Sensor-specific floors

| Sensor/profile | Minimum sample | Wake/power timing |
|---|---:|---|
| SHT31 | 100 ms | AlwaysOn class |
| Wind Rev C | 10 ms | 10 s wake delay; WarmupHeavy |
| SPS30 | 1,000 ms | 8 s wake delay; WarmupHeavy |
| ICM-20948 | 10 ms | no wake delay; DutyCycled |
| GPS continuous | 100 ms | no wake delay |
| GPS periodic | 1,000 ms | 24 s run / 90 s sleep |
| GPS AlwaysLocate | 1,000 ms | driver-managed |

## PowerConfig

Battery ADC uses 3.3 V reference, 10-bit maximum 1023, and a 2.0 divider ratio. The mapped battery range is 3.2–4.2 V, with 3.5 V as the low threshold, sampled no faster than once per second.

## BaseConfig

| Constant | Value | Purpose |
|---|---:|---|
| `kUartBaud` | 115,200 | Native USB CDC bridge |
| `kAckSummaryMinIntervalMs` | 25 ms | Coalescing/pacing |
| `kMaxAckSummarySendAttempts` | 3 | Base-window attempts before hold |
| `kAckSummaryNodeSilenceMs` | 9,000 ms | Two frames; sleeping-node fallback |
| `kMaxPendingCommandSendAttempts` | 3 | Local radio-queue refusals, not missing remote ACKs |
| `kPeriodicTimeSyncMs` | 50,000 ms | Base LoRa broadcast cadence |
| `kHealthLogPeriodMs` | 5,000 ms | Base health log |
| `kMaxAssignedNodes` | 4 | Derived from slots minus base |
| `kFirstNodeId` | 2 | First assigned node |
| `kMaxAckTrackedNodes` | 16 | Tracker hard capacity |

### Dynamic TX power

| Constant | Value |
|---|---:|
| SNR demod floor | -7.5 dB (stored as -75 tenths) |
| Target SNR margin | 10 dB |
| Downward step | 2 dBm |
| Dead band above target | 3 dB |
| Minimum decision interval | 60 s |
| Command ACK timeout | 120 s |
| Silence timeout | 300 s |
| Maximum silence probes | 3 |

These are shipped starting values, not bench-characterized thresholds. The controller steps down only with excess margin and jumps upward for recovery. A stale-sync node restores the 13 dBm DYNAMIC baseline.

## Edge receiver defaults

The authoritative Python values are in `edge/edge-receiver/src/smartfires_edge/config.py`:

| Setting | Default |
|---|---|
| Base device / baud | `/dev/smartfires-base` / 115200 |
| Data directory | `/mnt/nvme_drive/data` |
| Packet-loss node list | 2, 3, 4 |
| Metrics flush | 10 s |
| Jetson TIME_SYNC injection | 600 s |
| Web bind / port | `0.0.0.0:8080` |
| Sniffer | disabled; 115200, 5 slots when enabled |
| Live visualizer rows | 20 |
| Optional anemometer | disabled; 9600 baud, address 1, 1 s poll |

`CLI_CMD_ACK_TIMEOUT_S` and `CLI_CALIBRATION_DURATION_S` remain defined legacy constants but no current CLI subcommand consumes them. Do not treat them as active operator behavior.

## Change checklist

1. Edit the authoritative config source.
2. Check compile-time assertions and all derived frame/window values.
3. If packet cadence or size changes, recalculate `BANDWIDTH_SCALING.md`.
4. If `NUM_SLOTS` changes, rebuild/reflash every network Feather and update edge sniffer geometry.
5. If a C++ protocol struct changes, update Python format strings/decoders and size checks in the same change.
6. Update current docs and their `last_verified` date after verifying them.
