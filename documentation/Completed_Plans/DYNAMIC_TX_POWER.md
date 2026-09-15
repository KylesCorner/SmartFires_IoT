---
name: dynamic-tx-power
description: Implementation record for the shipped base-owned per-node LoRa TX-power controller and operator controls.
category: plan-completed
status: historical
superseded_by: tunable-parameters
related_docs:
  - lora-vs-lorawan
  - packet-reliability
  - reset-reason-diagnostics
---

# Dynamic TX power — implementation record

## Audit result

Audited against the firmware and edge source on 2026-09-04. The planned feature is implemented end to end and is appropriate for `Completed_Plans/`.

Implemented:

- `TxPowerController` maintains independent SNR, link-counter, mode, command, and silence state for each assigned node.
- The base considers decisions on STATUS, uses SNR from all valid node frames, steps down slowly, and returns directly to the 13 dBm baseline when margin is low.
- Retry or failure growth inhibits a reduction without incorrectly treating an app-layer retry as proof that node uplink power is too low.
- `CMD_SET_TX_POWER` is mirrored in C++ and Python. It carries an absolute power request plus DYNAMIC/STATIC mode.
- The node clamps requests to 5–13 dBm, applies the radio setting, reports the applied value and mode in STATUS, and returns `CMD_ACK`.
- Commands are queued for slot 0 and sent without a RadioHead link ACK; application acknowledgement arrives later in the node's slot.
- Node reboot/AWAKEN, stale sync, command timeout, lost acknowledgement, and prolonged silence have bounded recovery behavior.
- The edge service relays commands, persists reported state, exposes `/api/tx_power`, and provides set/increase/decrease and Auto/Pin controls in the dashboard.
- Native `TxPowerController` tests cover the decision rule, timing gates, STATIC mode, command reconciliation, wraparound, and silence probes.

## Shipped policy

The current values live in `BaseConfig.h` and `NetworkConfig.h`; `TUNABLE_PARAMETERS.md` is the maintained reference.

| Setting | Current value |
|---|---:|
| Baseline and ceiling | 13 dBm |
| Floor | 5 dBm |
| Target margin above SF7 demodulation floor | 10 dB |
| Step-down dead band | 3 dB |
| Step-down size | 2 dBm |
| Minimum per-node decision interval | 60 s |
| Command acknowledgement timeout | 120 s |
| Silence probe threshold | 5 min |
| Maximum consecutive silence probes | 3 |

The controller sends absolute values. DYNAMIC owns automatic changes; STATIC pins the reported level until reboot or stale-sync recovery. STATUS is the authority for what the node actually applied.

## Deferred operational work

These are refinements, not missing implementation:

- Field-characterize the margin target, dead band, cadence, timeouts, and energy savings before treating the current conservative values as optimized.
- Keep 13 dBm as the ceiling unless module power-path, supply, thermal, antenna, jurisdiction, and radiated-power constraints are separately validated.
- Correlate reset cause and low-battery data with commanded power during field trials.
- Controller decisions and command lifecycle are available in structured logs, while the dashboard primarily shows node-reported applied power and mode. Add richer historical decision charts only if operators need them.

No additional feature work is required for the present deployment. Optional tuning can be reopened as a small measurement task rather than reviving the original implementation plan.
