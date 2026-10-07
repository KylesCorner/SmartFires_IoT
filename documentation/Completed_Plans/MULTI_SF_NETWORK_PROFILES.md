---
name: multi-sf-network-profiles
description: Implementation record for compile-time SF7, SF9, SF10, and SF12 network profiles shared by the base, nodes, sniffer, and edge tooling.
category: plan-completed
status: historical
related_docs:
  - bandwidth-scaling
  - tdma-protocol
  - packet-reliability
  - tunable-parameters
  - lora-vs-lorawan
  - duty-cycling
  - base-slot-overrun-fix
---

# Multi-SF network build profiles

Requested 2026-10-06 and implemented 2026-10-07. The SF12 modem tuple was amended before its first trial to 250 kHz and CR 4/5. This is the design and implementation record; the current architecture and user-reference documents describe the authoritative operating behavior. No hardware was built, flashed, or exercised as part of the documentation update, so the higher-SF timing values remain provisional until the acceptance work below is completed.

This plan supersedes the former standalone SF12 maximum-range plan. SF12 is now one member of a common profile system rather than a special fork.

## Implemented outcome

Selecting `sf7`, `sf9`, `sf10`, or `sf12` at build time must select a complete, internally consistent network configuration for:

- the base-station Feather;
- every sensor-node Feather;
- the passive-sniffer Feather; and
- the Jetson's timing/visualization expectations.

The profile must cover more than the modem spreading factor. It must include every parameter whose safe value changes with airtime or frame length: modem registers, operational bundle size, TDMA geometry, TX admission/deadlines, sensing cadence, STATUS cadence, reliability timers, join cadence, time-sync cadence, duty-cycle drain behavior, watchdog assumptions, dynamic-power SNR reference, and edge/sniffer timing.

The intended operator workflow is to choose one named profile, build matching artifacts for all radio roles, and flash the whole radio fleet during one maintenance operation. Mixed profiles on one carrier are unsupported.

## Resolved architecture decisions

- Every deployment profile supports the existing five-slot geometry: one base slot plus four node slots. There is no separate small-fleet or two-slot SF12 profile in this scope.
- Selecting an SF also selects safe sensing, STATUS, bundle, and duty-timing defaults. The duty behavior itself remains a separate selector.
- Deployment node builds support **Continuous** and **Timed** duty modes. SensorTriggered is not a supported production configuration.
- PlatformIO exposes one shared `SMARTFIRES_NETWORK_PROFILE` value used by every LoRa environment. Changing that single value switches the base, Continuous node, Timed node, debug/development node, and sniffer builds together without adding an SF environment matrix.
- The base announces its compiled profile over USB and that announcement is authoritative for Jetson visualization, timing expectations, and session metadata. JSON/CLI remains available as an explicit recovery override.

## Implemented architecture

### 1. One typed profile definition

The data-only `platformio/include/config/NetworkProfiles.h` contains:

- a stable profile enum/ID for SF7, SF9, SF10, and SF12;
- a `NetworkProfile` aggregate with shared radio, TDMA, reliability, base, and node-cadence fields;
- one constexpr definition for each supported profile; and
- a selector driven by one build macro, provisionally `SMARTFIRES_NETWORK_PROFILE`.

`NetworkConfig.h`, `SensingConfig.h`, and `BaseConfig.h` should derive their active values from the selected profile. They should not each implement a separate `#if SF == ...` table. Role-specific config stays in its existing domain header, but the values that must move together originate in the one selected aggregate.

Keep board pins, packet format limits, calibration data, and unrelated sensor electrical limits outside the profile. Those values do not change merely because SF changes.

### 2. Separate wire limits from operating limits

Keep `BinaryPacket::kBundleMaxDeltas == 14` and the existing 195-byte maximum application frame as the decode/wire-compatibility ceiling.

The selected profile supplies operational `maxBundleDeltas` to `PacketHandler`. SF7, SF9, and SF10 use the full 14 deltas/15 samples. SF12 retains 7 deltas/8 samples as conservative first-trial headroom; at 250 kHz the larger wire-ceiling frame also fits the guarded five-second slot, but is not the normal transmit target.

This avoids a protocol fork: every decoder still accepts the existing maximum, while an SF12 sender deliberately closes a bundle sooner.

### 3. Calculate airtime and admit by deadline

Packet-type-only TX estimates were replaced with one tested airtime function using actual application length plus RadioHead's four-byte header and the selected modem tuple.

Before every base or node send:

1. calculate the frame's worst-case airtime;
2. add the profile's configured provisional software/radio completion margin;
3. compare that deadline with the trailing slot guard; and
4. send only if the complete operation fits, otherwise defer it.

The same calculation should set or validate the bounded local `waitPacketSent()` timeout. A profile must never allow the operational bundle cap to exceed its guarded slot. A received or accidentally constructed packet up to the larger wire ceiling remains decodable, but it is not automatically admissible for transmission.

### 4. Configure the modem explicitly

The driver explicitly applies and reports:

- spreading factor;
- profile-selected bandwidth (125 kHz for SF7/SF9/SF10; 250 kHz for SF12);
- coding rate 4/5;
- eight-symbol preamble;
- explicit header mode;
- payload CRC; and
- low-data-rate optimization when required.

Do not use RadioHead's `Bw125Cr48Sf4096` shortcut for SF12 because it selects 125 kHz and CR 4/8 instead of the desired 250 kHz and CR 4/5. Use explicit modem registers/configuration so the complete selected tuple is applied consistently.

All four profiles keep 915 MHz, CR 4/5, the current preamble, and the current 13 dBm ceiling. SF7/SF9/SF10 use 125 kHz; SF12 uses 250 kHz. Further changes to bandwidth, coding rate, carrier, preamble, or power ceiling are a separate profile-design exercise.

### 5. Use one shared PlatformIO selector

The existing role-oriented environment names remain. The profile selector is defined once in the shared `[network]` flags consumed by all LoRa roles:

```ini
[network]
build_flags =
  -DNUM_SLOTS=5
  -DSMARTFIRES_NETWORK_PROFILE=7
```

The operator changes only `SMARTFIRES_NETWORK_PROFILE` to `7`, `9`, `10`, or `12`, then builds the existing environments:

```text
feather_m0_lora_base
feather_m0_lora_node
feather_m0_lora_node_timed
feather_m0_lora_sniffer
```

`feather_m0_lora_node` remains Continuous and `feather_m0_lora_node_timed` remains Timed. The SF selector supplies each role's safe radio/load defaults while the node environment supplies only the duty behavior.

Every environment that participates in LoRa communication, including debug or retained development modes, must consume `${network.build_flags}`. Power-test and native-test environments do not select a deployed profile; native tests instantiate and validate all four definitions directly.

The build rejects a missing or unsupported selector instead of silently falling back to a different SF. Firmware startup logs and the profile announcement include its ID and fingerprint.

Because environment names no longer contain SF, any packaging/deployment helper should copy build outputs to profile-bearing artifact names such as `smartfires-base-sf9.bin` and `smartfires-node-continuous-sf9.bin`. It must read the currently selected value, show the matching base/node/sniffer set before flashing, and reject artifacts whose embedded manifests disagree. Automatic multi-device upload is useful, but is not required to establish the config architecture.

### 6. Advertise profile identity

Base and node startup logs identify the stable profile ID and fingerprint together with SF/BW/CR, slot geometry, operational bundle cap, sample cadence, and STATUS cadence. The sniffer emits equivalent profile identity and geometry in its startup `config` event.

The base emits the selected profile to the Jetson over the existing USB framing/control channel at startup and every five seconds. This is the authoritative runtime source for the edge process. The announcement contains the profile ID/fingerprint and the display/interpretation fields the edge needs; the Jetson does not maintain a second hardcoded SF table to decode it.

The edge process must:

- cache the most recent valid base announcement and expose it through the web API;
- drive dashboard sample expectations and TDMA visualization from it;
- show the active SF persistently in the dashboard header/status area, preferably as `SF9 · 125 kHz · 4/5`, with the full profile details available nearby;
- show `SF unknown` rather than assuming SF7 before the first valid announcement;
- indicate whether the value came from the base or from an explicit recovery override; and
- display a prominent warning if an override or sniffer-reported profile disagrees with the base.

`SessionMetaLogger` should persist a `network_profile` object in the existing `session.json`. At minimum it should record profile ID, SF, bandwidth, coding rate, slot count/width/guard, operational bundle cap, Continuous and Timed sample periods, STATUS interval, profile fingerprint, source (`base` or `override`), and the UTC time it was first observed. Session metadata should start with an explicit unknown/null state if logging begins before the base announcement, then be updated atomically when the announcement arrives.

Once telemetry has been recorded under a known profile, a different profile announcement must not silently overwrite that session's identity. Treat it as a configuration transition: raise a dashboard/log warning and start a new session or record a clearly bounded profile-history transition before accepting more data. Normal deployment should change SF only between sessions.

Generated session exports and analysis bundles must carry the same profile metadata, either embedded in a format that supports metadata or as a required sidecar manifest. Repeating SF on every telemetry CSV row is unnecessary while the session is guaranteed to have one profile, but a CSV must not be distributed without its session/profile metadata.

This removes today's manually mirrored `DEFAULT_NUM_SLOTS` and JavaScript sample-period assumptions from normal operation.

Changing the over-air binary packet layout solely to carry the profile ID is not required in the first implementation. A future protocol revision can add it if automatic mismatch detection on nodes is worth the compatibility cost.

## Provisional profile sets

These are conservative starting points for a base plus four simultaneously active nodes (`NUM_SLOTS=5`). They are not field-approved values. Airtime uses the standard LoRa formula with a four-byte RadioHead header, explicit header, payload CRC, eight preamble symbols, the profile bandwidth, and CR 4/5.

### Radio, bundle, and TDMA geometry

| Parameter | SF7 | SF9 | SF10 | SF12 |
|---|---:|---:|---:|---:|
| Profile selector | `7` | `9` | `10` | `12` |
| SF / BW / CR | 7 / 125 kHz / 4/5 | 9 / 125 kHz / 4/5 | 10 / 125 kHz / 4/5 | 12 / 250 kHz / 4/5 |
| Low-data-rate optimization | off | off | off | on |
| Typical demodulation SNR floor for power-margin math | -7.5 dB | -12.5 dB | -15 dB | -20 dB |
| Total slots | 5 | 5 | 5 | 5 |
| Slot width | 900 ms | 1,300 ms | 2,200 ms | 5,000 ms |
| Guard at each edge | 20 ms | 30 ms | 50 ms | 100 ms |
| Frame period | 4.5 s | 6.5 s | 11 s | 25 s |
| Operational deltas / samples | 14 / 15 | 14 / 15 | 14 / 15 | 7 / 8 |
| Maximum operational application bytes | 195 | 195 | 195 | 111 |
| Maximum operational BUNDLE airtime | 317.7 ms | 1,004.5 ms | 1,804.3 ms | 2,215.9 ms |
| Guarded usable slot | 860 ms | 1,240 ms | 2,100 ms | 4,800 ms |
| Raw margin after maximum BUNDLE | 542.3 ms | 235.5 ms | 295.7 ms | 2,584.1 ms |
| RX wake-ahead starting point | 150 ms | 200 ms | 250 ms | 300 ms |

The raw margin is not all usable scheduling time; it must also cover driver/software latency and measured oscillator/clock error. The SF9, SF10, and SF12 guards and wake-ahead values require hardware characterization.

### Offered load and node cadence

| Parameter | SF7 | SF9 | SF10 | SF12 |
|---|---:|---:|---:|---:|
| Continuous sample period | 750 ms | 750 ms | **1 s** | **4 s** |
| Timed sample period | 1 s | 1 s | 1 s | **4 s** |
| Full-bundle interval while continuously sampling | 11.25 s | 11.25 s | 15 s | 32 s |
| STATUS interval | 15 s | **30 s** | **120 s** | **300 s** |
| Approx. aggregate first-send uplink airtime, four active nodes | 13.2% | 39.0% | 49.6% | 28.9% |
| Long-frame opportunity headroom from packet production | 42.9% | 25.9% | 21.2% | 15.7% |

The opportunity calculation conservatively assumes at most one queued frame is serviced per node slot even when multiple small frames could fit. It includes BUNDLE and STATUS production but not retries, joins, window markers, commands, or base traffic. Those loads are why each higher-SF profile retains explicit headroom rather than being sized exactly at its theoretical limit.

Sample-rate consequences:

- SF7 keeps the current cadence.
- SF9 can keep the 750 ms production cadence if STATUS is reduced from 15 to 30 seconds.
- SF10 should drop production sampling from 750 ms to 1 second and reduce STATUS to every two minutes.
- SF12 cannot sustain the current data rate for four nodes. It should start at one sample every four seconds, eight samples per bundle, and a five-minute STATUS interval.

At the current 750 ms cadence, four SF10 nodes would consume nearly their complete equal TDMA share before control traffic and retries. At SF12/250 kHz, a 15-sample BUNDLE takes about 3.609 seconds and now fits the five-second slot, but four nodes producing one every 11.25 seconds would still demand well over the channel's capacity. The eight-sample operating cap and slower cadence are retained as conservative first-trial headroom.

### Reliability, control, and duty timing

| Parameter | SF7 | SF9 | SF10 | SF12 |
|---|---:|---:|---:|---:|
| App reliability | `ACK_SUMMARY` | `ACK_SUMMARY` | `ACK_SUMMARY` | `ACK_SUMMARY` |
| Retry wait (two frames) | 9 s | 13 s | 22 s | 50 s |
| Pending maximum age starting point | 30 s | 40 s | 70 s | 150 s |
| AWAKEN repeat | ~5 s | ~7 s | ~12 s | ~27 s |
| Periodic base TIME_SYNC | 50 s | 65 s | 110 s | 125 s |
| Maximum final TX drain before standby | 5 s | 8 s | 15 s | 30 s |
| Timed active sampling for two complete bundles | 30 s | 30 s | 30 s | 64 s |
| Timed wake-to-wake cycle starting point | 75 s | 80 s | 90 s | 150 s |

AWAKEN must add UID-derived jitter/backoff rather than have newly powered nodes repeat in lockstep. Retry, silence, command, and join timers should be represented as frame multiples plus bounded margins wherever their semantics are frame-based; the table shows the resulting human-readable starting values.

Timed duty-cycle windows should close on whole operational bundles. Warmup-heavy sensors, especially the SPS30, still need their existing electrical/warmup minima, so slower radio sampling does not automatically imply proportional battery savings.

## Cross-cutting implementation

### Remove scheduled blocking link ACKs

The scheduled blocking-link-ACK paths were removed consistently across all profiles:

- send `ACK_SUMMARY` fire-and-forget and rely on its repeated cumulative state;
- send direct assignment `TIME_SYNC` fire-and-forget; an unassigned node keeps requesting until it receives one;
- send `AWAKEN` fire-and-forget, treating assignment `TIME_SYNC` as the application response;
- retain application `CMD_ACK` for commands; and
- do not use `StrictLinkAck` in the four deployment profiles.

This becomes mandatory at high SF. A one-byte RadioHead ACK body takes about 414 ms at SF12/250 kHz, still longer than the current 250 ms receive timeout, and a blocking retry burst cannot be contained safely in the proposed schedule.

### Dynamic TX power

Move the demodulation floor into the selected profile. The currently hardcoded -7.5 dB reference is specific to SF7 and would overestimate required power at higher SF.

Use static 13 dBm during initial A/B range and profile acceptance testing so power control does not confound results. Re-enable dynamic control profile-by-profile only after its SNR thresholds, decision interval, silence timeout, and command-ACK timeout have been exercised with the longer frames.

### Watchdog and blocking work

Audit every bounded or blocking operation against the selected maximum airtime. The modeled SF12 operational BUNDLE airtime is now 2.216 seconds at 250 kHz, leaving substantially more of the current eight-second steady watchdog for sensor work and scheduling jitter. Feed immediately before and after legitimate bounded radio operations, never inside a potentially hung wait; change the watchdog timeout only if measurement shows it is necessary.

### Edge and sniffer

- Apply the selected modem tuple to the sniffer firmware.
- Replace hardcoded 900 ms/20 ms sniffer geometry with the announced profile.
- Replace the dashboard's hardcoded 750 ms expected sample period with profile data.
- Show the active SF/profile and its source persistently on the dashboard, including unknown and mismatch states.
- Store the complete active profile object in `session.json` and propagate it to exported/archived session data so captures remain interpretable later.
- Reject silent mid-session profile replacement; warn and create an explicit session/metadata boundary.
- Scale healthy command, join, silence, and retry UI expectations to the active profile.
- Retain explicit overrides, but warn when they disagree with the base announcement.

## Compile-time and host-test invariants

Define all four profiles in every native build and test all of them, rather than compiling tests only for the currently selected profile.

Required invariants include:

- selector is one of 7, 9, 10, or 12;
- slot count supports the declared base plus node count;
- operational bundle deltas do not exceed the 14-delta wire ceiling;
- calculated maximum operational airtime plus guards and configured margin fits its slot;
- generated BUNDLE + STATUS packet rate remains below the conservative service rate with declared headroom;
- retry wait is at least one complete frame and maximum age permits all configured attempts;
- final TX drain can span a worst-phase wait to the node's next slot plus one admitted transmission;
- SNR floor is supplied for every profile;
- low-data-rate optimization is enabled when the selected symbol duration requires it;
- base and node configs consume the same geometry object; and
- serialized/announced profile fields round-trip correctly on the edge;
- dashboard/API state distinguishes base-announced, overridden, mismatched, and unknown profiles; and
- session metadata updates atomically from unknown to known but cannot silently replace a known profile after telemetry begins.

Add golden airtime cases for every packet class and each SF. Keep the existing Python/HTML calculator synchronized from the same documented assumptions, but do not make generated web data the firmware's source of truth.

## Implementation summary

The implementation now includes all four constexpr profiles, explicit modem programming, separate wire and operational bundle limits, actual-length airtime admission, nonblocking scheduled sends, profile-derived cadence/reliability/duty values, one shared PlatformIO selector, the base-to-edge announcement, edge/API/session state, dashboard profile display, sniffer geometry consumption, host tests, and current documentation.

Profile-bearing artifact packaging and automatic multi-device flashing were not added; operators still build and flash the existing role environments after changing the shared selector. Hardware validation also remains outstanding. Bench and soak SF7, SF9, SF10, and SF12 in increasing-airtime order; a successful SF12 trial does not prove the shorter profiles because their tighter slots exercise different deadline margins.

## Hardware validation and acceptance still required

For each profile:

1. Measure airtime for BUNDLE at the operational maximum and for every control/status packet.
2. Confirm node and base sends finish inside guarded slots under simultaneous sensor work.
3. Exercise one node, then four continuously active nodes, then synchronized wake/trigger bursts.
4. Exercise simultaneous cold boot, assignment, lost sync, base restart, node sleep/wake, commands, reset, dropped BUNDLEs, and dropped ACK summaries.
5. Run at least a 24-hour four-node soak and show queues/pending windows remain bounded without a steady drop trend.
6. Measure delivered-sample ratio, end-to-end latency, retries, join time, watchdog resets, and energy.
7. Perform controlled range comparisons using the same boards, antennas, power, mounting, route, and weather conditions.
8. Exercise rollback to the known-good SF7 artifacts.
9. Confirm the dashboard shows the correct SF/source, shows unknown before announcement, and warns on simulated base/override/sniffer disagreement.
10. Confirm every completed session and exported analysis bundle records the profile used, including a session that starts before the first base announcement and a simulated mid-session profile change.

Adopt a profile for field use only when it meets the required range/reliability target and its sample rate, latency, and energy cost are acceptable. The preferred operating profile should be the lowest SF that meets the deployment requirement, not automatically SF12.

Outdoor/production use still requires deployment-specific review of frequency, EIRP/antenna, channel occupancy/dwell behavior, and equipment authorization. This plan does not infer compliance from use of 915 MHz.

## Edge profile authority

The base-announced profile is authoritative, with an explicit JSON/CLI recovery override:

- it prevents the dashboard's sample cadence and sniffer geometry from silently disagreeing with the flashed radio fleet;
- changing SF does not require finding and updating a second timing table on the Jetson;
- the received profile ID and full interpretation fields are displayed and stored automatically with every session; and
- an override still allows recovery when the base is disconnected or running older firmware.

This announcement is only on the base-to-Jetson USB link. It does not change the LoRa packet format or add airtime.

## Out of scope for the first implementation

- mixed-SF nodes in one TDMA frame;
- adaptive data rate or changing SF at runtime;
- SensorTriggered or Hybrid deployment variants for every SF;
- changing bandwidth, coding rate, carrier, preamble, or maximum TX power;
- changing the binary BUNDLE layout or 14-delta decoder ceiling;
- frequency hopping, CAD, mesh, or LoRaWAN; and
- claiming a range multiplier before controlled measurements.
