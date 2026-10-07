---
name: tdma-protocol
description: Slot geometry, session clock, boot handshake, and TX budget for the TDMA radio layer.
category: architecture
status: current
last_verified: 2026-10-07
source_refs:
  - platformio/include/radio/TdmaConfig.h
  - platformio/include/config/NetworkProfiles.h
  - platformio/include/config/NetworkConfig.h
  - platformio/include/radio/LoRaAirtime.h
  - platformio/src/radio/TdmaClock.cpp
  - platformio/src/radio/TdmaRadioService.cpp
  - platformio/include/radio/ITdmaRadioDriver.h
  - platformio/src/platform/RadioHeadTdmaDriver.cpp
related_docs:
  - packet-reliability
  - bandwidth-scaling
  - tunable-parameters
  - radio-rx-gating
---

# TDMA protocol

SmartFires divides one 915 MHz raw-LoRa channel into fixed, repeating time slots. TDMA governs steady-state traffic; joining and selected reset/recovery packets have explicit out-of-band behavior.

## Selected geometry

All four profiles use five slots: slot 0 belongs to base address/node ID 1 and four node slots begin at ID 2. The remaining timing moves with `SMARTFIRES_NETWORK_PROFILE`:

| Profile | Slot width | Guard at each edge | Usable window | Frame | RX wake-ahead |
|---|---:|---:|---:|---:|---:|
| SF7 | 900 ms | 20 ms | 860 ms | 4.5 s | 150 ms |
| SF9 | 1,300 ms | 30 ms | 1,240 ms | 6.5 s | 200 ms |
| SF10 | 2,200 ms | 50 ms | 2,100 ms | 11 s | 250 ms |
| SF12 | 5,000 ms | 100 ms | 4,800 ms | 25 s | 300 ms |

For session time `t`, the nominal slot is `(t / slot_width_ms) % 5`. `TdmaClock::myTurn()` applies the selected guards. Base and node consume the same `NetworkConfig::kGeometry`; `NUM_SLOTS=5` remains a build-time tripwire and compilation fails if it disagrees with the selected profile.

## Join before TDMA

A node cannot know its slot before it has an assigned ID. Joining therefore occurs outside normal slot gating:

1. The node hashes the SAMD21 UID and uses a temporary radio address.
2. It sends fire-and-forget `AWAKEN(uid_hash, reset_cause, hang_zone)` at the selected 5/7/12/27-second interval plus bounded UID/sequence-derived jitter.
3. The base finds or creates the persistent assignment and queues a fire-and-forget direct `TIME_SYNC` to the temporary radio address.
4. The sync header carries the assigned node ID. The node applies session time, changes its radio address, and begins sensing/TDMA. If the response is lost, the node remains unassigned and asks again.

The current `AWAKEN` is 12 bytes; the decoder still accepts the old 9-byte header/payload layout. Slot 0 is never assigned to a sensor node.

## Session clock

Packets use session-relative milliseconds rather than wall-clock time. The Jetson starts a random session ID and maps session time to UTC when ingesting. Its default USB TIME_SYNC interval is 600 seconds.

The base maintains a clock continuously. It caches Jetson time when available, otherwise falls back to its local session, and broadcasts fire-and-forget `TIME_SYNC` every 50/65/110/125 seconds for SF7/SF9/SF10/SF12. Nodes update their session clock on valid direct or broadcast sync.

Before the first sync and after 22 minutes without refresh, node transmit and receive gates become permissive. This costs power/channel discipline but ensures a stale node can hear recovery sync and rejoin.

## Slot traffic

Node slots carry BUNDLE, STATUS, FULL_STATE, queued `CMD_ACK`, window markers, and retransmissions. Current app-layer telemetry uses RadioHead `send()` rather than remote link ACK. The node's queue is drained only in its guarded slot while sync is fresh.

Slot 0 carries direct assignment sync, base-to-node commands, `ACK_SUMMARY`, and periodic broadcast sync, in that priority order. The base attempts one pending category per `update()` call, and subsequent loop iterations within the same slot may send more.

Commands are fire-and-forget and acknowledged later by `CMD_ACK`. Direct sync and `ACK_SUMMARY` are also fire-and-forget. Every scheduled base send passes the same remaining-slot deadline check used by node traffic.

## TX budget

TX admission is based on actual application length, the selected modem tuple, RadioHead's four-byte header, and a profile completion/software margin (22 ms at SF7; 50/50/100 ms at SF9/SF10/SF12). A scheduled send starts only when that total fits before the trailing guard. The same value bounds local TX completion.

The loop still caps each update at three sends and one retransmission per slot. Compile-time checks prove the selected operational maximum bundle fits, but the SF9/SF10/SF12 margins remain provisional until measured on hardware.

## Node receiver gating

Steady node uplinks do not need to listen for immediate link ACKs. The SX1276 receiver sleeps outside the base window and starts waking by the profile's 150/200/250/300 ms wake-ahead. That margin is distinct from clock-drift guards: it covers radio wake latency and main-loop jitter from blocking sensor reads.

The receiver remains available continuously while unsynchronized/stale and while the application must drain its final Timed-window queue. Direct commands can only be received in the base's slot; their response is scheduled in the node's slot, except reset ACK as documented in the reliability reference.

## Window markers

Timed nodes emit `WINDOW_BEGIN` and `WINDOW_END` without consuming telemetry reliability sequence numbers. `WINDOW_END` carries the planned standby duration and sample count. The base uses these markers to avoid sending acknowledgement summaries to a sleeping receiver; silence provides a fallback if the end marker is lost.

Markers are deliberately fire-and-forget. Their state is advisory and recoverable, while making them reliable would couple the sleep boundary to an acknowledgement that cannot arrive until after the node sleeps.

## Scaling rule

The shipped profiles are five-slot definitions. Supporting a different fleet size requires adding or revising a complete profile, rebuilding/reflashing the base, every node, and the sniffer, and then recalculating:

- frame period and per-node service rate;
- ACK/retry intervals and pending age;
- base assignment capacity;
- receiver wake, guarded admission, and completion margin;
- regulatory/channel-airtime limits.

See `BANDWIDTH_SCALING.md` for the current calculation and `PACKET_RELIABILITY.md` for ACK pacing.
