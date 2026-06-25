---
name: telemetry-rework-plan
description: Large-scale plan to move telemetry from text-based full-state streaming to a binary protocol with delta packets, periodic refreshes, Feather-side queueing, and session time sync.
category: plan-completed
status: historical
superseded_by: software-design
related_docs:
  - binary-packet-pipeline
---

# SmartFires Telemetry Rework Plan

## Purpose

This document lays out a large-scale plan for moving SmartFires telemetry from the current text-based full-state stream to a more robust binary protocol with:

- compact packet encoding
- delta packets plus periodic full-state refreshes
- Feather-side queueing for the slow LoRa link
- session-wide synchronized time via `session_time`
- room for future ACK, retry, and base-station control behavior

The goal is to reduce unnecessary bytes on the wire while preserving system correctness on a lossy, low-bandwidth LoRa link.

## Current State Summary

Current behavior:

- ESP32 samples sensors and builds a full telemetry snapshot every cycle.
- ESP32 encodes telemetry as ASCII CSV-like text.
- ESP32 sends telemetry to the Feather over UART.
- Feather parses the text packet, ACKs UART receipt, and forwards the same payload over LoRa.
- Feather currently has no outbound queue, no LoRa retry protocol, and no end-to-end state resync.

Current limitations:

- unchanged values are resent every time
- text encoding wastes bytes
- UART is faster than LoRa, so the bottleneck is on the Feather
- receiver state can drift if delta-style compression is added without periodic full refresh
- there is no global synchronized timeline shared between nodes

## Target Architecture

### ESP32 Responsibilities

The ESP32 should own sensor semantics and packet generation.

Responsibilities:

- collect raw sensor readings
- quantize sensor values into compact integer fields
- maintain current telemetry state
- maintain last acknowledged telemetry baseline for delta generation
- decide whether to emit a full packet, delta packet, event packet, or heartbeat
- attach `session_time` to each outgoing sample frame

The ESP32 should not own the LoRa transmission backlog. It should remain focused on sensing and building the smallest useful telemetry payload.

### Feather Responsibilities

The Feather should own rate control and LoRa transmission behavior.

Responsibilities:

- receive binary telemetry packets from the ESP32 over UART
- ACK UART receipt
- queue packets for LoRa transmission
- prioritize urgent packets over routine packets
- drop, replace, or coalesce stale routine telemetry if the queue grows
- relay base-station sync/control packets back toward the ESP32 when needed

The Feather should be the point where slow-link policy lives.

### Base Station Responsibilities

The base station should own session coordination.

Responsibilities:

- maintain the master session clock
- periodically broadcast time synchronization updates
- receive full and delta telemetry packets
- reconstruct node state from full packets plus deltas
- request resync if sequence gaps or state loss are detected

## Session Time Design

Every sample frame should include `session_time`.

Definition:

- `session_time` is a monotonically increasing master time for the active mission/session.
- units should be milliseconds unless packet-size constraints later force a coarser unit
- all nodes should treat it as the canonical time axis for comparing sensor intervals across devices

Why this matters:

- lets the base station compare measurements from different nodes on a common time axis
- makes inter-node event ordering possible
- decouples analytics from local MCU uptime and reboot timing

Recommended model:

- the base station maintains the authoritative session clock
- each node maintains a local estimate of the current session clock
- the base station periodically transmits sync packets
- nodes update their local session clock estimate from those packets
- all sample frames include the node's current `session_time` estimate at packet build time

Recommended fields for sync packets:

- session identifier
- master `session_time`
- sender transmit timestamp or sequence number
- optional flags for reset, new mission, or forced resync

Recommended node-side clock behavior:

- keep a local monotonic timer based on `millis()`
- maintain an offset from local monotonic time to master `session_time`
- update that offset when valid sync packets arrive
- avoid stepping backward unless the session is explicitly reset
- if a sync jump is large, mark it and treat it as a session correction event

Recommended first implementation:

- keep `session_time` as a `uint32_t` millisecond counter
- on sync, compute `session_time = local_millis + offset`
- if base station sends a newer authoritative value, adjust the offset
- if the base station signals a new session, reset offset and packet baselines

## Packet Strategy

The protocol should move from text lines to binary packets.

### Packet Types

Suggested packet types:

- `FULL_STATE`: complete self-contained state snapshot
- `DELTA_STATE`: only fields that changed enough to matter
- `EVENT_ALERT`: urgent packet for discrete state changes like flame detection
- `HEARTBEAT`: tiny keepalive/status frame
- `TIME_SYNC`: base-station packet carrying authoritative `session_time`
- `SYNC_REQUEST`: optional node request for resync after reset or gap detection

### Full Packets

Use full packets:

- on boot
- after rejoin or reconnect
- after sequence loss or decode uncertainty
- periodically, like an I-frame in a video stream

Recommended starting cadence:

- every 5 to 15 seconds
- immediately after major state changes if the receiver may need a clean baseline

### Delta Packets

Use delta packets for normal operation.

Rules:

- include a changed-fields bitmask
- only include fields whose value changed beyond a configured threshold
- compare against the last acknowledged full-or-delta baseline
- do not compare floating-point values directly; compare quantized integer values

### Event Packets

Use event packets for urgent state changes.

Examples:

- flame detected changed from false to true
- sensor failure or health state change
- GPS fix acquired or lost
- mission mode change

These packets should bypass stale queue items when appropriate.

## Field Encoding Strategy

Avoid transmitting floats where compact fixed-point integers are sufficient.

Suggested encodings:

- `session_time`: `uint32_t` milliseconds
- sequence number: `uint16_t` or `uint32_t`
- sensor flags: `uint16_t`
- flame state: `uint8_t`
- wind speed: `uint16_t` in cm/s or dm/s
- temperature: `int16_t` in centi-degrees C
- humidity: `uint16_t` in centi-percent
- lidar: `uint16_t` in centimeters
- GPS fix and satellites: packed `uint8_t` fields
- latitude and longitude: `int32_t` in degrees times `1e7`

This keeps the format deterministic and much smaller than text.

## GPS Optimization Strategy

GPS is a clear candidate for change-based suppression.

Recommended behavior:

- do not resend latitude and longitude on every routine packet
- include GPS coordinates only when movement exceeds a threshold or when a full-state packet is sent
- include GPS fix-state changes immediately
- include periodic full GPS refresh even if unchanged

Recommended first threshold:

- only resend coordinates when movement exceeds roughly 2 to 5 meters

Do not begin with delta-from-previous-coordinate compression. Start with absolute coordinates sent only when needed. It is simpler and more robust on a lossy link.

## Queueing Strategy On The Feather

The Feather should maintain a small outbound ring buffer for LoRa packets.

Recommended behavior:

- ACK UART receipt as soon as the packet is validated and enqueued
- maintain packet priority classes: event, full, delta, heartbeat
- if the queue is near full, drop or replace stale delta packets before dropping event packets
- optionally collapse multiple pending deltas into the newest state if they are superseded
- maintain queue metrics for debugging: depth, drops, merges, oldest age

Recommended first version:

- fixed-size ring buffer
- no dynamic allocation
- simple priority handling
- no end-to-end retry yet

## State And Sequence Model

The system should maintain explicit state baselines.

### On The ESP32

Maintain:

- `current_state`
- `last_uart_acked_state`
- `last_full_sent_state`
- `next_sequence`
- `current_session_time`

Important rule:

- only advance the delta baseline after UART ACK from the Feather, not immediately after local send

### On The Base Station

Maintain:

- latest reconstructed node state
- last sequence seen per node
- whether current node state is trusted or degraded
- last full-state arrival time
- session clock authority

If sequence gaps are detected repeatedly, mark that node as requiring a full-state refresh.

## Recommended Rollout Phases

### Phase 1: Binary Full-State Packets

Replace text telemetry with binary full-state packets while keeping system behavior otherwise simple.

Deliverables:

- define binary packet header
- define binary full-state payload
- update ESP32 encoder
- update Feather decoder
- preserve current UART ACK behavior

Success criteria:

- system works end-to-end with smaller packets than the current text format
- packet parsing is deterministic

### Phase 2: Add Session Time Synchronization

Add base-station-driven synchronized time.

Deliverables:

- define `TIME_SYNC` packet
- add node-side session clock estimator
- attach `session_time` to every outgoing sample frame
- define new-session/reset behavior

Success criteria:

- sensor frames from different nodes can be compared using the same session timeline

### Phase 3: Add Delta Packets ✓ Done

Add changed-field suppression while keeping periodic full-state refreshes.

Deliverables:

- quantized telemetry state struct ✓
- `PKT_BUNDLE`: reference `FullStatePayload` + up to 7 `DeltaPayload` entries ✓
- GPS coordinates separated into one-time `PKT_GPS` per session (saves 8 bytes/packet) ✓
- 4 Hz sensing rate; bundle accumulates every 2 s at N_δ=7 ✓
- periodic full-state refresh via the bundle reference frame ✓

**TODO — startup TIME_SYNC before first reading:**
Currently the ESP32 starts sending telemetry immediately, using `millis()` as `session_time`
until the first TIME_SYNC arrives (~30 s). `uptime_ms` is retained in `FullStatePayload`
partly to make this pre-sync period diagnosable. Once TIME_SYNC is forced to complete before
the ESP32 begins sampling, `uptime_ms` becomes redundant with `session_time` and can be
dropped — saving 4 bytes per packet (141 → 137 bytes at N_δ=7; recalculate slot sizing).
Requires handshake or delay logic on both the node Feather and ESP32 at boot.

Success criteria:

- normal packets are significantly smaller than full-state packets ✓ (17.6 bytes/sample vs 36)
- receiver stays correct across packet loss due to periodic full refresh ✓

### Phase 4: Add Feather Queueing

Add outbound queueing and stale-packet handling on the Feather.

Deliverables:

- fixed-size transmit ring buffer
- packet priority policy
- queue depth and drop counters
- stale delta replacement logic

Success criteria:

- LoRa link no longer blocks UART handling directly
- system remains stable when telemetry production exceeds LoRa send capacity

### Phase 5: Add Receiver Recovery Features

Add stronger state recovery and operator visibility.

Deliverables:

- sequence-gap handling rules
- optional `SYNC_REQUEST`
- debug counters for full frames, deltas, drops, resyncs, sync jumps
- optional end-to-end LoRa ACK design if needed later

Success criteria:

- system degrades gracefully under loss and recovers automatically

## File-Level Impact

Expected main areas of change:

- `src/shared/TelemetryPacket.h`
- `src/shared/TelemetryCodec.h`
- `src/shared/UartLoRaBridge.h`
- `src/drone/main.cpp`
- `src/lora_feather/main.cpp`

Likely additions later:

- a shared binary packet definition header
- a shared time sync header or merged control-packet definitions
- a small Feather queue implementation file

## Open Design Decisions

These should be decided before deep implementation:

- exact packet header format
- sequence number width
- exact `session_time` unit and rollover policy
- full-state resend cadence
- per-field delta thresholds
- queue depth on Feather
- whether UART ACK means parsed, enqueued, or actually sent over LoRa
- whether base station sends sync by broadcast only or also per-node repair messages

## GPS Optimization Strategy ✓ Done

GPS coordinates are transmitted once per session via `PKT_GPS` (not in every packet).
The receiver caches the last GPS fix per node and injects it into every CSV row.
Rows before the first GPS fix have empty lat/lon fields.
A new session ID (receiver.py restart) causes nodes to re-send GPS on their next valid fix.

## Recommended First Concrete Step

Implement binary full-state packets first, without delta suppression yet, but with `session_time` already included in the header or payload. That keeps the first cut simple while establishing the packet format, the synchronized-time model, and the UART/Feather plumbing needed for the later delta and queueing work.
