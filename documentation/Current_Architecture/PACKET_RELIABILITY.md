# Packet Reliability

SmartFires uses a two-layer reliability architecture that keeps radio-link
control entirely on the Feather boards and excludes the Jetson from the
acknowledgement exchange.

## Packet Classification

| Packet | Direction | Reliability | Rationale |
|---|---|---|---|
| `AWAKEN` | Node → Base | Link-layer ACK (`sendToWait`) | Boot-critical; node must know base is alive |
| `TIME_SYNC` | Base → Nodes | Fire-and-forget broadcast | Periodic; next sync supersedes a missed one |
| `BUNDLE` / `STATUS` | Node → Base | Configurable (see below) | Telemetry — governed by reliability mode |
| `ACK_SUMMARY` | Base → Node | Fire-and-forget | Periodic summary; loss costs one retry cycle |

## Reliability Modes

The telemetry send path has two modes, selected at build time via the
`SMARTFIRES_TDMA_RELIABILITY_MODE` compile flag. All production and debug
node environments currently build with mode `1`.

### Mode 0 — StrictLinkAck

Each telemetry packet waits for a RadioHead link-layer acknowledgement before
the TX slot is released. If the ACK is not received within `ackTimeoutMs`,
the driver retries up to `maxRetries` times before declaring failure.

| Parameter | Value |
|---|---:|
| `enableLinkAck` | `true` |
| `maxRetries` | 3 |
| `ackTimeoutMs` | 250 ms |

A failed packet (no ACK after all retries) is dropped. There is no app-layer
retransmit in this mode.

**Use case**: diagnostics, dense lab testing where packet loss needs to be
immediately visible in serial logs.

### Mode 1 — AppLayerAckSummary (current production mode)

Telemetry packets are sent fire-and-forget at the LoRa link layer (`send()`,
no per-packet ACK wait). The base still auto-ACKs via RadioHead's receive path
(`recvfromAck()`), but the node does not block on this.

Reliability is recovered at the application layer through a **pending window**
on the node and periodic **ACK_SUMMARY** frames generated locally by the base
station firmware.

## App-Layer Reliability Mechanism

### Node Side — Pending Window

After a fresh telemetry packet is sent, `TdmaRadioService` stores a copy in a
pending window. When idle TX time is available in a later slot, the node
re-sends the oldest unacknowledged entry from this window.

| Parameter | Value | Notes |
|---|---|---|
| `kMaxReliabilityWindow` | 8 | Hard cap in firmware |
| `reliabilityWindowDepth` | 8 | Effective window size (configurable) |
| `reliabilityMaxAttempts` | 3 | Pending entry dropped after this many retransmits |
| `reliabilityMaxAgeMs` | 30 000 ms | Pending entry dropped after 30 s regardless of attempts |
| `reliabilityMinRetryGapMs` | 2 000 ms | Minimum gap between retransmits of the same seq |
| `reliabilityFreshTrafficHoldoffMs` | 2 000 ms | Retransmits are suppressed for 2 s after a fresh send |

Fresh queue entries always take priority over retransmits. Retransmits only
fill slots when the TX queue is empty.

#### ACK-Paced Retry Gate

Before a pending entry becomes eligible for retransmission, `TdmaRadioService`
computes a retry-wait duration and requires that much time to have elapsed
since the entry was last sent:

```
retryWaitMs = clamp(expectedAckIntervalMs * retryWaitMultiplierPermille / 1000,
                     retryWaitMinMs, retryWaitMaxMs)
```

| Parameter | Value | Notes |
|---|---|---|
| `expectedAckIntervalMs` | 4 000 ms | Expected cadence of base-side `ACK_SUMMARY` packets |
| `retryWaitMultiplierPermille` | 2000 (2.0×) | Back-off multiplier applied to the expected interval |
| `retryWaitMinMs` | 4 500 ms | Floor: one interval + 500 ms jitter margin |
| `retryWaitMaxMs` | 10 000 ms | Ceiling: 2.5 intervals |

At current values, `retryWaitMs` evaluates to `4000 × 2.0 = 8000`, clamped into
`[4500, 10000]` → **8 000 ms**. An entry younger than this is skipped by
`pickRetransmitCandidate()` regardless of queue idleness.

`requireAckSummaryBeforeFirstRetry` (currently `false`) optionally gates a
pending entry's *first* retransmit attempt on having observed at least one
`ACK_SUMMARY` since the entry was sent, with a fallback: once the entry's age
reaches `retryWaitMaxMs`, the gate is bypassed so the entry is not stuck
forever if no `ACK_SUMMARY` ever arrives.

The pending window uses **sequence-number matching** to avoid keeping duplicate
entries. If the same seq arrives again (e.g., re-enqueued after a slot defer),
the entry is refreshed in place.

When the window is full and a new entry must be added, the entry with the most
retransmit attempts and greatest age is evicted first.

### Base Side — ACK_SUMMARY

`SmartFiresBaseApp` tracks received telemetry sequence numbers per node in a
sliding window. Whenever it receives a standard telemetry packet (`STATUS`,
`BUNDLE`, `FULL_STATE`), it updates the per-node tracker, marks the tracker
dirty, and later emits a targeted `ACK_SUMMARY` in the base TDMA slot window.

The `ACK_SUMMARY` wire format encodes this as:

```
AckSummaryPayload (4 bytes)
  node_id:      uint8_t   — target node
  ack_base_seq: uint8_t   — highest contiguous seq acknowledged
  ack_mask:     uint16_t  — bit N set means (ack_base_seq + N + 1) is acked
```

ACK summaries are paced on the base side with a small minimum flush interval
and are only sent during the base slot window. The Jetson does not generate or
forward standard-packet acknowledgements.

### Node Side — Applying ACK_SUMMARY

`TdmaRadioService::applyAckSummary()` walks the pending window and marks each
entry acknowledged if its sequence number falls within the summary's
coverage. Acknowledged entries are immediately freed from the window.

Sequence number comparison uses 8-bit modulo arithmetic:

- If `(ack_base_seq − seq) < 128`: seq is at or before base → acknowledged.
- If `1 ≤ (seq − ack_base_seq) ≤ 16`: check the corresponding mask bit.

## Reliability Boundary

The Feather-to-Feather LoRa link owns reliability. The base station receives
telemetry, updates its local ACK tracker, and sends `ACK_SUMMARY` packets back
to the node. The Jetson receives forwarded telemetry and is not on the
acknowledgement path for standard packets.

## History

The current design replaced an earlier approach where all telemetry used
blocking `sendToWait` for every packet. See
[Completed_Plans/NETWORK_RELIABILITY_NOTES.md](../Completed_Plans/NETWORK_RELIABILITY_NOTES.md)
and [Completed_Plans/PHASE_PROGRESS.md](../Completed_Plans/PHASE_PROGRESS.md)
for the staged migration history.
