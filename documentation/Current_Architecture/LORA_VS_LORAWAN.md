---
name: lora-vs-lorawan
description: Why SmartFires uses a custom RadioHead/TDMA stack instead of LoRaWAN, mesh capability, CAD, and the optimization/range levers available on the current radio.
category: architecture
status: current
last_verified: 2026-09-04
source_refs:
  - platformio/src/platform/RadioHeadTdmaDriver.cpp
  - platformio/include/config/NetworkConfig.h
related_docs:
  - tdma-protocol
  - packet-reliability
  - tunable-parameters
  - dynamic-tx-power
---

# LoRa versus LoRaWAN

SmartFires uses raw LoRa modulation through RadioHead `RH_RF95`/`RHReliableDatagram` plus a project-owned TDMA and binary protocol. It is not a LoRaWAN network.

## Current stack

| Layer | SmartFires implementation |
|---|---|
| Radio | Feather M0 RFM95 / SX1276 family |
| Carrier | 915 MHz |
| Modem | RadioHead default `Bw125Cr45Sf128` profile unless the driver is changed |
| Power | 13 dBm baseline, base-controlled 5–13 dBm per node |
| Addressing | Base address 1; node IDs assigned from UID hashes |
| Medium access | Five-slot TDMA, 900 ms per slot |
| Reliability | App-layer cumulative ACK summaries for telemetry; selected RadioHead link ACKs for control |
| Security | No LoRaWAN join/encryption/authentication layer |

RadioHead's “reliable datagram” class provides addressing, packet IDs, and optional ACK/retry primitives. SmartFires deliberately bypasses remote link ACK for steady telemetry and implements its own bounded retry window so one lost ACK cannot block sensing or consume an entire slot.

## Why a custom link fits this prototype

- The system has a small, known fleet and a dedicated local base.
- Runtime UID assignment and fixed TDMA slots are controlled end-to-end.
- The binary protocol carries project-specific bundles, sleep markers, resets, debug logs, and TX-power feedback without a network server.
- The Jetson can operate locally without internet or LoRaWAN infrastructure.

The cost is that SmartFires owns collision behavior, replay/authentication concerns, device provisioning, retry policy, regional configuration, observability, and interoperability. “Raw LoRa” is a radio choice, not a complete production network standard.

## What LoRaWAN would change

LoRaWAN would replace most of the custom MAC and join/security layer with standardized device activation, session keys, frame counters, uplink/downlink classes, regional channel plans, gateways, and network/application servers. It would improve interoperability and ecosystem tooling.

It would also require redesigning the current deterministic slot/ACK/window protocol. A standard LoRaWAN gateway would not simply relay `BinaryPacket` frames or preserve SmartFires node IDs and ACK summaries. Downlink timing, duty cycling, payload limits, and deployment operations would move into LoRaWAN's model.

Whether that trade is worthwhile depends on fleet size, security requirements, gateway/network-server availability, and certification/deployment goals; it is not a firmware flag.

## Collision claims and out-of-band traffic

TDMA separates synchronized steady telemetry by assigned slot, but the system is not literally contention-free:

- unassigned nodes send `AWAKEN` outside normal slot ownership;
- reset ACK is intentionally immediate;
- stale/unsynchronized nodes use permissive recovery behavior;
- a known blocking base send can overrun slot 0;
- external 915 MHz emitters do not honor the schedule.

The correct claim is deterministic scheduled access under fresh sync, not guaranteed zero collisions.

## CAD and mesh

Channel Activity Detection (CAD) asks the LoRa modem whether a LoRa preamble-like signal is present. The current SmartFires transmission path does not perform listen-before-talk CAD. Adding it could reduce collisions during join/recovery, but it cannot reserve the channel, authenticate a peer, or replace the TDMA schedule.

The network is a star: nodes talk to one base, and neither nodes nor base route third-party packets. The SX1276 can support protocols that implement relaying, but mesh would require routing, duplicate suppression, hop limits, timing/power policy, retransmission ownership, and new capacity analysis. No current code provides those layers.

## Range and energy levers

The main levers are antenna quality/placement, line of sight, spreading factor, bandwidth, coding rate, transmit power, receiver time, payload size, retry behavior, and fleet geometry. They trade airtime, energy, latency, and robustness; changing modem settings requires matching every radio and recalculating packet airtime and slot budgets.

Dynamic TX power is already shipped: the base records SNR on every assigned-node frame, considers STATUS retry/failure deltas, and sends absolute 5–13 dBm commands. Operators can pin STATIC power or restore DYNAMIC control in the dashboard. Thresholds are not yet field-tuned, and the current ceiling intentionally remains the known 13 dBm baseline rather than the radio's maximum capability.

Do not infer regional compliance from the 915 MHz carrier alone. Antenna gain, radiated power, dwell time/channel use, and operating jurisdiction all matter and must be checked for the actual deployment.
