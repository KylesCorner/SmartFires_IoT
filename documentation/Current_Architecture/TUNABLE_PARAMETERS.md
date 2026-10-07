---
name: tunable-parameters
description: Every tunable constant in the system — TDMA, sensing/duty-cycle, power, Jetson.
category: architecture
status: current
last_verified: 2026-10-07
source_refs:
  - platformio/platformio.ini
  - platformio/include/config/NetworkProfiles.h
  - platformio/include/config/NetworkConfig.h
  - platformio/include/config/SensingConfig.h
  - platformio/include/config/PowerConfig.h
  - platformio/include/config/BaseConfig.h
  - edge/edge-receiver/src/smartfires_edge/config.py
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
| `SMARTFIRES_NETWORK_PROFILE` | 7 | Selects complete SF7/SF9/SF10/SF12 network values; shared by every LoRa role |
| `NUM_SLOTS` | 5 | Tripwire that must match the selected profile |
| `SMARTFIRES_DUTY_CYCLE_MODE` | 0 production, 2 debug/timed, 3 hybrid | 0 Continuous, 1 SensorTriggered, 2 Timed, 3 Hybrid |

Deployable radio builds reject a missing or unsupported network selector. Native and power-test builds use SF7 only as a fixture. Change the one selector, then rebuild the base, every node role, and the sniffer; mixed profiles are unsupported.

## NetworkConfig

### Profile-derived network values

| Constant family | SF7 | SF9 | SF10 | SF12 |
|---|---:|---:|---:|---:|
| Slot / guard / frame | 900 / 20 / 4,500 ms | 1,300 / 30 / 6,500 ms | 2,200 / 50 / 11,000 ms | 5,000 / 100 / 25,000 ms |
| RX wake-ahead | 150 ms | 200 ms | 250 ms | 300 ms |
| Operational deltas / bytes | 14 / 195 | 14 / 195 | 14 / 195 | 7 / 111 |
| Max BUNDLE airtime / TX margin | 318 / 22 ms | 1,005 / 50 ms | 1,805 / 50 ms | 2,216 / 100 ms |
| Continuous / Timed sample | 750 / 1,000 ms | 750 / 1,000 ms | 1,000 / 1,000 ms | 4,000 / 4,000 ms |
| STATUS interval | 15 s | 30 s | 120 s | 300 s |
| Retry wait / maximum age | 9 / 30 s | 13 / 40 s | 22 / 70 s | 50 / 150 s |
| AWAKEN / periodic TIME_SYNC | 5 / 50 s | 7 / 65 s | 12 / 110 s | 27 / 125 s |
| Final TX drain / Timed active / cycle | 5 / 30 / 75 s | 8 / 30 / 80 s | 15 / 30 / 90 s | 30 / 64 / 150 s |
| SNR demod floor | -7.5 dB | -12.5 dB | -15 dB | -20 dB |
| Command ACK timeout | 120 s | 120 s | 120 s | 180 s |

Every profile also fixes five slots, a 22-minute sync-stale timeout, app-layer ACK summaries, and three total telemetry attempts.

### Shared radio and queue values

| Constant | Value | Notes |
|---|---:|---|
| `kBaseAddr` | 1 | Base radio address/node ID |
| `kRadioFrequencyMhz` | 915.0 | Raw LoRa carrier |
| `kRadioTxPowerDbm` | 13 dBm | Boot/baseline power |
| Modem tuple | SF7/SF9/SF10: 125 kHz; SF12: 250 kHz; all CR 4/5, preamble 8, explicit header, payload CRC | SF12 alone enables low-data-rate optimization |
| `kMinTxPowerDbm` / `kMaxTxPowerDbm` | 5 / 13 dBm | Node clamp for control commands |
| `kLinkRetries` | 3 | RadioHead retries after first attempt |
| `kLinkAckTimeoutMs` | 250 ms | Remote ACK wait |
| TX completion | actual packet airtime + profile margin | Used for admission and bounded local wait |

### Queue and reliability

| Constant | Value |
|---|---:|
| `kQueueDepth` | 8 |
| `kReliabilityWindowDepth` | 8 |
| `kReliabilityMaxAttempts` | 3 total attempts in every profile |
| `kReliabilityMinRetryGapMs` | 2,000 ms |
| `kReliabilityFreshTrafficHoldoffMs` | 2,000 ms |
| `kExpectedAckIntervalMs` | selected frame period |
| `kRetryWaitMultiplierPermille` | 2,000 = 2.0x |
| `kRetryWaitMinMs` / `Max` | selected retry wait, fixed at two frames |
| `kRequireAckSummaryBeforeFirstRetry` | false |
| `kAwakenIntervalMs` | selected profile plus bounded UID/attempt jitter |
| `kEnableTelemetryTx` | true |

The retry floor must cover at least one frame. Compile-time checks also enforce supported SF, modem tuple, operational bundle ceiling/slot fit, retry age, final drain, and whole-bundle Timed windows.

## SensingConfig

### Controller profiles

| Profile | Warmup | Sample | Active | Scheduled period | Other |
|---|---:|---:|---:|---:|---|
| Continuous | 10 s | profile Continuous value | unbounded | none | production target; no intentional sleep |
| SensorTriggered | 10 s | fixed 750 ms | 30 s | none | retained unsupported deployment mode; 3 s minimum sleep; 1 °C / 5 %RH trigger |
| Timed | 10 s | profile Timed value | 30 s, except SF12 64 s | 75/80/90/150 s | whole operational bundles; 5 s minimum standby |
| Hybrid | 10 s | fixed 750 ms | 30 s | 340 s | retained unsupported deployment mode; trigger or timer |

The minimum worthwhile MCU standby is 250 ms. Profile-derived final TX drain is 5/8/15/30 seconds. A full operational bundle is 15 samples except SF12's eight; Timed windows close on two complete bundles.

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
| `kAckSummaryNodeSilenceMs` | two selected frames | Sleeping-node fallback |
| `kMaxPendingCommandSendAttempts` | 3 | Local radio-queue refusals, not missing remote ACKs |
| `kPeriodicTimeSyncMs` | selected 50/65/110/125 s | Base LoRa broadcast cadence |
| `kHealthLogPeriodMs` | 5,000 ms | Base health log |
| `kMaxAssignedNodes` | 4 | Derived from slots minus base |
| `kFirstNodeId` | 2 | First assigned node |
| `kMaxAckTrackedNodes` | 16 | Tracker hard capacity |

### Dynamic TX power

| Constant | Value |
|---|---:|
| SNR demod floor | profile-specific: -7.5/-12.5/-15/-20 dB |
| Target SNR margin | 10 dB |
| Downward step | 2 dBm |
| Dead band above target | 3 dB |
| Minimum decision interval | 60 s |
| Command ACK timeout | 120 s; SF12 180 s |
| Silence timeout | 300 s |
| Maximum silence probes | 3 |

These are shipped starting values, not bench-characterized thresholds. The controller steps down only with excess margin and jumps upward for recovery. SF7 defaults to DYNAMIC; SF9/SF10/SF12 default STATIC at 13 dBm for acceptance trials. A stale-sync node restores the profile default.

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
| Network profile override | none; recovery-only JSON/CLI override when explicitly supplied |
| Sniffer | disabled; 115200; announced geometry overrides fallback timing when enabled |
| Live visualizer rows | 20 |
| Optional anemometer | disabled; 9600 baud, address 1, 1 s poll |

`CLI_CMD_ACK_TIMEOUT_S` and `CLI_CALIBRATION_DURATION_S` remain defined legacy constants but no current CLI subcommand consumes them. Do not treat them as active operator behavior.

## Change checklist

1. Edit the authoritative config source.
2. Check compile-time assertions and all derived frame/window values.
3. If packet cadence or size changes, recalculate `BANDWIDTH_SCALING.md`.
4. For any profile change, rebuild/reflash every network Feather; the edge learns geometry from the base announcement.
5. If a C++ protocol struct changes, update Python format strings/decoders and size checks in the same change.
6. Update current docs and their `last_verified` date after verifying them.
