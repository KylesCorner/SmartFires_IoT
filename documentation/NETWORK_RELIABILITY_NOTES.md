# Network Reliability Notes

## Current Situation

This note captures the current state of the Feather-to-Feather LoRa link after
the AWAKEN/TIME_SYNC handshake debugging session.

At the end of this session:

- The dummy node successfully sends `AWAKEN`.
- The base Feather receives `AWAKEN`.
- The base Feather sends `TIME_SYNC` back to the node.
- The node receives `TIME_SYNC` and transitions into normal telemetry flow.
- The dummy node produces real telemetry and transmits `STATUS` / `BUNDLE`
  packets after sync.

This means the control-plane handshake is working.

## Key Finding

The link behaved much more reliably when packets were sent with RadioHead
link-layer acknowledgement (`sendToWait`) instead of fire-and-forget
transmission (`sendto`).

Important interpretation:

- This does **not** mean the radios can only communicate in ACK mode.
- It means the current RF link is fragile enough that best-effort transmit is
  not trustworthy for diagnostics.
- `sendToWait` improves observed behavior because it adds:
  - delivery confirmation
  - automatic retry
  - tighter transaction pacing

During this session, sending all node uplinks with link-layer ACK was used as a
diagnostic tool to determine whether packets were truly reaching the base
 Feather.

## What We Observed

The following behaviors were established during testing:

- Small control packets (`AWAKEN`, `TIME_SYNC`) can complete successfully.
- Once synced, the dummy node does generate and transmit real telemetry.
- When all uplinks use link-layer ACK, telemetry delivery becomes visible and
  diagnosable.
- The earlier missing-data problem was therefore not a node sampling problem.
- The main uncertainty is long-term telemetry strategy, not whether the node can
  produce packets.

## Temporary Diagnostic State

The code was temporarily biased toward reliable diagnostics:

- `AWAKEN` uses link-layer ACK.
- `TIME_SYNC` direct response from the base uses link-layer ACK.
- For diagnostics, node telemetry sends were also forced through link-layer ACK
  so the logs can show `link_ack=OK` / `link_ack=NO` on actual data packets.

This is useful for debugging, but it is **not** the desired final behavior for a
responsive low-power node.

## Why This Is Not the Final Design

Blocking on `sendToWait` for every telemetry packet can tie up the node and make
it less available for sensing, queue management, and future features.

Long-term goal:

- Keep the node responsive.
- Avoid blocking on every telemetry send.
- Preserve reliability where it matters.

## Decision For Next Session

Next session should focus on reducing payload pressure and moving reliability to
the correct boundary.

### 1. Reduce packet size

Primary next experiment:

- Reduce `BUNDLE` size.
- Lower the number of deltas carried per bundle.
- Prefer smaller, more reliable packets over large packets that need retries.

Reason:

- The handshake packets are small and work reliably.
- Telemetry packets are larger and are the more likely source of RF fragility.

### 2. Move away from blocking ACK waits for telemetry

Target shape:

- Keep link-layer ACK for critical control packets only.
  - `AWAKEN`
  - `TIME_SYNC`
- Do **not** rely on blocking `sendToWait` for normal telemetry in the final
  design.

### 3. Keep reliability on the Feather boards only

Planned architecture decision:

- Reliability exchange should live between the Feather node and Feather base.
- The Jetson should **not** be part of that reliability loop.

Meaning:

- The Jetson can remain responsible for ingest, logging, and downstream data use.
- The Jetson should not be required to complete the radio reliability exchange.
- Retransmission and acknowledgement decisions should be made on the Feather
  side of the system.

This keeps radio-link control closer to the radios and reduces dependency on the
Jetson path for basic delivery guarantees.

## Planned Direction

The preferred next-step architecture is:

- `AWAKEN`: reliable, link-layer ACK
- `TIME_SYNC`: reliable, link-layer ACK
- telemetry (`STATUS`, `BUNDLE`): non-blocking send in normal operation
- reliability bookkeeping: Feather node <-> Feather base
- Jetson: out of the radio reliability exchange

## Next Session Checklist

1. Reduce telemetry packet size, especially `BUNDLE` payload depth.
2. Re-test without forcing all telemetry through blocking ACK waits.
3. Keep control-plane packets reliable.
4. Rework reliability ownership so it stays on the Feather boards only.
5. Verify that the Jetson is no longer required for the telemetry reliability
   handshake.

## Summary

Current session outcome:

- Handshake works.
- Telemetry generation works.
- ACK-for-everything is useful diagnostically.
- ACK-for-everything is not the intended final architecture.

Next session outcome target:

- Smaller telemetry packets.
- Non-blocking telemetry sends.
- Feather-only reliability logic.
- Jetson removed from the reliability exchange.