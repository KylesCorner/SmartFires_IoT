---
name: base-slot-overrun-fix
description: Deferred hardening for blocking base transmissions and fragile Timed-window markers.
category: plan-possible
status: deferred
related_docs:
  - window-marker-packets
  - packet-reliability
  - tdma-protocol
  - duty-cycling
  - dynamic-tx-power
---

# Possible: eliminate base slot overruns

## Current gap

The base still uses `sendToWait()` for `ACK_SUMMARY` and direct assignment `TIME_SYNC`. With three RadioHead retries and a 250 ms acknowledgement timeout, one unreachable destination can occupy roughly 1000 ms. That cannot fit inside the 900 ms slot or its 860 ms guarded transmit span.

`baseTxWindowOpen()` only checks whether slot 0 is open when a send starts; it does not check whether enough time remains. A lost `WINDOW_END` can also leave the base believing a Timed node is awake, causing an acknowledgement attempt while that node's radio is off.

Commands are already fire-and-forget plus `CMD_ACK`; they are not part of this blocking-send defect.

## If resumed

1. Make `ACK_SUMMARY` fire-and-forget and stop link-ACKing it on nodes. Its cumulative bitmap naturally recovers a lost summary.
2. Make direct assignment `TIME_SYNC` fire-and-forget as well. A node already repeats `AWAKEN` every 5 seconds until sync arrives.
3. Add a shared airtime-budget function and a base deadline gate so no send starts unless it fits before the trailing guard.
4. Consider two cheap in-slot copies of `ACK_SUMMARY` and `WINDOW_END` to reduce avoidable retransmissions without blocking.
5. Keep pending work queued when the deadline gate defers it.

The preferred invariant is: no base slot-0 path waits for a remote link ACK, and no node transmits a link ACK inside the base's slot.

## Verification before deployment

- Add unit coverage for remaining-slot math and base acknowledgement state.
- Confirm with the passive sniffer that base frames never cross into slot 1.
- Compare telemetry `retx` density and `drop_pending reason=max_attempts` before and after.
- Confirm repeated window markers do not create `tx_drain_timeout` or shorten standby.
- Reflash the base before nodes if link-ACK behavior changes.

This is deferred hardening. Resume it if slot-overrun or sleeping-node acknowledgement failures are observed again.
