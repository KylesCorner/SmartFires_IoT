---
name: sf12-max-range-trial
description: Plan for an SF12 maximum-range experiment, including airtime, TDMA, bundle, reliability, duty-cycle, power, and validation changes.
category: plan-pending
status: draft
related_docs:
  - bandwidth-scaling
  - tdma-protocol
  - packet-reliability
  - tunable-parameters
  - lora-vs-lorawan
  - duty-cycling
  - radio-rx-gating
  - watchdog-timer
  - base-slot-overrun-fix
---

# SF12 maximum-range trial

Requested and planned 2026-09-30. This is a planning document only: no firmware, edge, or deployment behavior has been changed, and no hardware command has been run.

## Decision summary

Test the SX1276's highest spreading factor, **SF12**, as a distinct, reversible maximum-range profile. Keep the other modem variables at the current values for the first experiment: 915 MHz, 125 kHz bandwidth, coding rate 4/5, explicit header, payload CRC, and the normal eight-symbol preamble. Pin every test node at the current maximum allowed power of 13 dBm. This isolates the effect of spreading factor; RadioHead's `Bw125Cr48Sf4096` preset must not be used accidentally because it also changes coding rate from 4/5 to 4/8.

SF12 cannot be dropped into the current 900 ms schedule. At the existing modem settings a maximum 195-byte BUNDLE takes about 317.7 ms. At SF12 the same application frame takes about 7.217 s, approximately 22.7 times longer. Even the minimum 27-byte BUNDLE or a STATUS frame takes about 1.810 s. The current packet cadence, TX budgets, ACK timeouts, reliability timers, watchdog assumptions, and five-second join loop therefore cannot remain as they are.

The recommended first multi-node profile is deliberately conservative but still needs measurement before adoption:

| Setting | Current | SF12 trial starting point |
|---|---:|---:|
| Spreading factor / bandwidth / coding rate | SF7 / 125 kHz / 4/5 | **SF12 / 125 kHz / 4/5** |
| TX power | Dynamic, 5–13 dBm | **Static 13 dBm during range trials** |
| Slot count | 5 | 5 (base plus four node slots) |
| Slot width / guard | 900 / 20 ms | **5,000 / 100 ms provisional** |
| Frame period | 4.5 s | **25 s** |
| Operational bundle cap | 14 deltas / 15 samples / 195 bytes | **7 deltas / 8 samples / 111 bytes** |
| Bundle airtime | 194.8 ms at the proposed 111-byte size | **4.432 s** |
| Active sample period | 750 ms production; 1 s Timed | **4 s starting point** |
| Full-bundle production interval | 11.25 s production | **32 s** |
| STATUS interval | 15 s | **300 s** |
| Normal sends in a node slot | Up to three; two maximum bundles fit | **Normally one long frame; a second small frame only if measured deadline permits** |
| Telemetry retry wait | 9 s | **Frame-derived, initially two frames = 50 s** |
| Maximum pending age | 30 s | **At least 150 s; finalize from soak results** |
| Periodic LoRa TIME_SYNC | 50 s | **About five frames = 125 s starting point** |
| Final Timed-window TX drain | 5 s | **At least one frame plus TX margin, about 30 s, or replace with phase-aware drain behavior** |

These values are a test hypothesis, not field-approved operating values. In particular, the 100 ms guard and approximately 368 ms margin between a 4.432 s BUNDLE and the 4.8 s guarded window must be verified on hardware under sensor load and clock drift.

## Complete before/after change matrix

The tables below are the implementation checklist. “Unchanged” is stated where a nearby parameter could otherwise be changed accidentally. Provisional values require the bench and soak gates later in this plan; they are not deployed defaults.

### Radio and packet profile

| Parameter or design | Current SF7 system | Proposed SF12 trial | Required action or reason |
|---|---|---|---|
| Carrier | 915 MHz | 915 MHz, unchanged | Isolate spreading factor; regional compliance remains deployment-specific. |
| Spreading factor | Implicit RadioHead SF7 default | Explicit SF12 | Configure and report it on base, nodes, and sniffer. |
| Signal bandwidth | 125 kHz | 125 kHz, unchanged | RF occupied bandwidth does not shrink in this trial. |
| Coding rate | 4/5 | 4/5, unchanged | Do not use the RadioHead SF12 preset that also selects 4/8. |
| Preamble/header/CRC | 8 symbols, explicit header, CRC on | Unchanged | Keeps the comparison focused on SF. |
| Low-data-rate optimization | Off at SF7 | On at SF12/125 kHz | RadioHead enables it when SF12 is selected; verify register state. |
| Baseline TX power | 13 dBm; dynamic controller may reduce to 5 dBm | Static 13 dBm during range trials | Prevent power control from confounding range results. |
| Dynamic-power SNR floor | `-7.5 dB`, explicitly SF7-specific | Disabled for trial; SF12 floor must be derived before re-enabling | The current controller would misinterpret SF12 link margin. |
| Binary BUNDLE decode ceiling | 14 deltas / 15 samples / 195 application bytes | Unchanged | Preserve wire compatibility and decoder safety. |
| Operational transmit bundle | Same as decode ceiling | 7 deltas / 8 samples / 111 application bytes | Fit one bounded long frame and reduce loss blast radius. |
| Packet sizing model | Packet-type fixed budgets | Actual application length plus four RadioHead bytes and modem settings | Prevent an oversized BUNDLE from crossing a slot. |

### Traffic and duty cycling

| Parameter or design | Current SF7 system | Proposed SF12 trial | Required action or reason |
|---|---|---|---|
| Continuous/SensorTriggered/Hybrid sample period | 750 ms | 4 s starting point | Offered traffic must fall with the lower modem rate. |
| Timed sample period | 1 s | 4 s starting point | Use the same measured trial cadence unless a separate profile is justified. |
| Samples per full bundle | 15 | 8 | Derive from operational bundle target, not wire maximum. |
| Full-bundle production interval | 11.25 s at 750 ms; 15 s in Timed mode | 32 s | Keeps production below one normal opportunity per 25-second frame. |
| STATUS cadence | 15 s | 300 s | STATUS alone would otherwise consume substantial SF12 airtime. |
| SensorTriggered active window | 30 s, not expressed as the proposed bundle unit | Whole 32- or 64-second bundle window | Avoid a routine partial bundle at sleep. |
| Timed active sampling | 30 s / two 15-sample bundles | About 64 s / two 8-sample bundles | Maintain two complete bundles at the slower cadence. |
| Timed wake-to-wake cycle | 75 s | At least 150 s initially | Leave room for warmup, sampling, drain, and meaningful standby. |
| Timed final TX drain | 5 s | About 30 s, or phase-aware drain | A node may wait almost one 25-second frame for its slot. |
| Partial-bundle behavior | Forced only on overrun/close path | Same safety behavior, but should be exceptional | Validate that normal windows end on the new full-bundle boundary. |

### TDMA and transmission deadlines

| Parameter or design | Current SF7 system | Proposed SF12 trial | Required action or reason |
|---|---|---|---|
| Total slots | 5 | 5, unchanged | Retain capacity for base plus four assigned nodes. |
| Slot width | 900 ms | 5,000 ms provisional | A 111-byte SF12 BUNDLE takes 4.432 s. |
| Guard at each edge | 20 ms | 100 ms provisional | Re-measure with clock drift, sensor blocking, and radio latency. |
| Usable slot span | 860 ms | 4,800 ms | Leaves about 368 ms beyond calculated BUNDLE airtime. |
| Frame period | 4.5 s | 25 s | Drives service, ACK, command, silence, and retry timing. |
| Maximum normal node sends | Up to three; two maximum bundles fit by budget | One long frame normally | Allow a second short frame only through deadline calculation. |
| Maximum BUNDLE TX budget | Fixed 340 ms | Measured length-aware budget, provisionally 4.6–4.7 s for 111 bytes | Include software/radio margin without admitting a 195-byte frame. |
| Local TX-completion wait | 340 ms for all sends | Length-aware; greater than measured ToA plus margin | Prevent sleep or a later radio call from aborting an in-flight frame. |
| Node RX wake-ahead | 150 ms | 250–300 ms provisional | Bench-characterize radio wake plus main-loop jitter. |
| Base slot admission | Window-open check; blocking paths can overrun | Airtime-aware trailing-deadline gate | No base frame may cross into slot 1. |
| Sniffer geometry | Hardcoded 900 ms / 20 ms in Python and browser | Configured 5,000 ms / 100 ms | Keep visualization and guard diagnostics aligned with firmware. |

### Reliability, join, and control plane

| Parameter or design | Current SF7 system | Proposed SF12 trial | Required action or reason |
|---|---|---|---|
| Telemetry reliability | App-layer cumulative `ACK_SUMMARY` | Unchanged concept | Retain cumulative acknowledgement and bounded pending storage. |
| `ACK_SUMMARY` transport | Blocking `sendToWait()` plus node link ACK | Fire-and-forget | A lost cumulative summary is recoverable; blocking is not slot-safe. |
| Direct assignment `TIME_SYNC` | Blocking `sendToWait()` plus node link ACK | Fire-and-forget | The node continues requesting assignment until sync arrives. |
| `AWAKEN` transport | Blocking `sendToWait()` | Fire-and-forget | Assignment `TIME_SYNC` becomes the completion signal. |
| Commands / `CMD_ACK` | Fire-and-forget command plus application response | Same model | Retune latency expectations; no RadioHead ACK is added. |
| RadioHead remote-ACK timeout | 250 ms | Not used by the SF12 operational profile | An SF12 RadioHead ACK itself takes about 827 ms. |
| RadioHead retry count | Three retries on selected link-ACK paths | No scheduled link-ACK retries | Repetition/recovery occurs at the application layer. |
| StrictLinkAck mode | Available diagnostic alternative | Prohibited in SF12 trial profile | Its worst-case exchange does not fit the proposed schedule. |
| Expected ACK interval | One 4.5-second frame | One 25-second frame | Derive rather than copy a literal. |
| Telemetry retry wait | 9 s | Initially two frames / 50 s | Give the base time to rotate/coalesce summaries. |
| Reliability maximum attempts | Three total | Three total initially | Preserve the policy while spacing attempts to the new frame. |
| Maximum pending age | 30 s | At least 150 s, final value from soak | Must outlive the planned retry opportunities and Timed sleep. |
| Queue and pending depth | Eight each | Eight each initially | Validate SRAM and pressure before considering enlargement. |
| Sleeping-node silence | Two frames / 9 s | Two frames / about 50 s | Preserve the semantic rule in frame units. |
| `AWAKEN` repetition | Every 5 s, deterministic | Roughly every frame with randomized jitter/backoff | Avoid overlapping 1.319-second join packets from rebooted nodes. |
| Periodic LoRa `TIME_SYNC` | Every 50 s | About five frames / 125 s initially | Reduce control load while retaining many chances before stale sync. |
| Sync-stale threshold | 22 min | 22 min initially | Validate recovery because permissive stale traffic is costlier at SF12. |
| ACK-summary scheduling | Paces at 25 ms and rotates dirty nodes | Airtime/deadline paced; may rotate over multiple base slots | Millisecond pacing is no longer the limiting resource. |

### Edge, health, deployment, and validation

| Parameter or design | Current SF7 system | Proposed SF12 trial | Required action or reason |
|---|---|---|---|
| Edge `DEFAULT_NUM_SLOTS` | 5 | 5, unchanged | Slot count remains synchronized. |
| Edge expected sample interval | Browser assumes 750 ms | 4 s profile value | Prevent false missing-data indications. |
| Command/UI timeout expectations | Tuned around short frames; some legacy constants remain | Frame-derived, tens of seconds | Healthy commands may require a full frame each way. |
| Steady watchdog | 8 s | Retain only if measured loop margin is safe; otherwise retune deliberately | A normal long TX is 4.432 s before sensor work and overhead. |
| Startup diagnostics | Frequency/power and timing logs, no complete modem identity | Log SF/BW/CR/preamble, geometry, bundle cap, and cadence | Detect mixed or partially flashed profiles. |
| Deployment | Existing SF7 fleet | Atomic base/node/sniffer SF12 maintenance window | Mixed profiles are unsupported; keep SF7 recovery images onsite. |
| Regulatory assumption | No compliance inferred from carrier | Formal review before outdoor/production use | Multi-second transmissions, antenna gain, EIRP, and channel use matter. |
| Rollback | Reflash existing profile | Exercised return to the known-good SF7 profile | Rollback is part of acceptance, not an untested contingency. |

## Airtime calculation

The calculation includes the four-byte RadioHead header in addition to the SmartFires application frame. It assumes SF12, 125 kHz, CR 4/5, explicit header, CRC enabled, eight preamble symbols, and low-data-rate optimization. RadioHead 1.120 automatically enables the low-data-rate bit when SF12 is selected at 125 kHz.

For BUNDLE, the SmartFires application length is `27 + 12 * delta_count` bytes. The important design points are:

| Deltas | Samples | Application bytes | RadioHead/PHY payload bytes | SF12 airtime |
|---:|---:|---:|---:|---:|
| 0 | 1 | 27 | 31 | 1.810 s |
| 3 | 4 | 63 | 67 | 2.957 s |
| **7** | **8** | **111** | **115** | **4.432 s** |
| 10 | 11 | 147 | 151 | 5.743 s |
| 14 | 15 | 195 | 199 | 7.217 s |

Control-plane airtime also becomes material:

| Frame | Application bytes | SF12 airtime |
|---|---:|---:|
| RadioHead ACK body | 1 | 0.827 s |
| `ACK_SUMMARY` | 10 | 1.155 s |
| 8- or 9-byte command | 8–9 | 1.155 s |
| `AWAKEN` / `CMD_ACK` | 12 | 1.319 s |
| `TIME_SYNC` | 14 | 1.319 s |
| Window marker | 17 | 1.483 s |
| `STATUS` / minimum BUNDLE | 27 | 1.810 s |

The present 250 ms remote-ACK timeout is shorter than the approximately 827 ms ACK transmission itself. Increasing only that timeout would make blocking retries consume several seconds and cross slot boundaries. The SF12 design should instead remove remote link-ACK waits from scheduled operation and rely on the protocol's existing cumulative/application responses.

## Effective bandwidth and SF comparison

“Bandwidth” has two different meanings here. The **occupied RF channel width stays 125 kHz** in both profiles. What collapses at higher SF is symbol rate, nominal modem bit rate, and delivered application goodput.

Use the standalone [`LoRa + TDMA design calculator`](../Tools/lora_tdma_calculator.html) to vary these inputs and see packet airtime, effective goodput, TDMA utilization, energy, warnings, and multiple node-count bounds update together.

### Current versus proposed profile

| Capacity measure | Current SF7 profile | Proposed SF12 profile | Effect |
|---|---:|---:|---:|
| Occupied RF bandwidth | 125 kHz | 125 kHz | Unchanged |
| Symbol duration / rate | 1.024 ms / 976.6 symbols/s | 32.768 ms / 30.52 symbols/s | Symbols are 32 times longer |
| Nominal LoRa bit rate at CR 4/5 | 5.469 kbps | 0.293 kbps | 18.7 times lower |
| Effective application-wire goodput for a 111-byte BUNDLE | 4.558 kbps | 0.200 kbps | 22.75 times lower |
| 111-byte BUNDLE airtime | 194.8 ms | 4,431.9 ms | 22.75 times longer |
| Configured sensor rate per active node | 1.333 samples/s | 0.250 samples/s | 5.33 times lower |
| Offered application-wire rate per node, BUNDLE plus STATUS | 153.1 bps | 28.47 bps | 5.38 times lower |
| First-transmission airtime per continuously active node | 3.30% | 14.45% | 4.37 times higher despite slower sensing |
| First-transmission airtime for four active nodes | 13.21% | 57.81% | Base/control/retries are additional |
| Configured/deployment node capacity | 4 | 4 trial target | SF12 requires the reduced-load profile to retain four |

Application-wire goodput counts the complete SmartFires frame as useful bytes and divides it by packet airtime; it is not sensor-only entropy or a promise of sustained network throughput. Preamble, LoRa coding, RadioHead headers, guards, base traffic, and idle portions of assigned slots account for the difference from nominal bit rate.

### SF7 through SF12 trade space

The next table holds frequency, 125 kHz bandwidth, CR 4/5, preamble, header, and CRC constant. “Current load” means a 195-byte BUNDLE every 11.25 seconds plus a 27-byte STATUS every 15 seconds. “Reduced load” means the proposed 111-byte BUNDLE every 32 seconds plus STATUS every 300 seconds.

The node ceilings are **airtime-only upper bounds for equal-slot TDMA with one equal share reserved for the base**:

```text
per_node_airtime = bundle_airtime / bundle_period + status_airtime / status_period
upper_bound_nodes = floor(1 / per_node_airtime - 1)
```

They assume perfect packing, no guards, no markers, no joins, no commands, no sync, no retries, no interference, and no software limits. They are comparison figures, not supported fleet sizes.

| SF | Nominal bit rate | 111-byte BUNDLE airtime | 111-byte effective goodput | Current-load airtime/node | Current-load airtime-only node bound | Reduced-load airtime/node | Reduced-load airtime-only node bound |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 7 | 5.469 kbps | 0.195 s | 4.558 kbps | 3.30% | 29 | 0.63% | 157 |
| 8 | 3.125 kbps | 0.349 s | 2.547 kbps | 5.90% | 15 | 1.13% | 87 |
| 9 | 1.758 kbps | 0.615 s | 1.443 kbps | 10.57% | 8 | 2.01% | 48 |
| 10 | 0.977 kbps | 1.149 s | 0.773 kbps | 19.06% | 4 | 3.74% | 25 |
| 11 | 0.537 kbps | 2.462 s | 0.361 kbps | 41.75% | 1 | 7.99% | 11 |
| 12 | 0.293 kbps | 4.432 s | 0.200 kbps | 76.22% | 0 | 14.45% | 5 |

The SF12 `0` under current load means the current traffic cannot fit even one node while preserving an equal base slot: one node alone needs 76.22% airtime but a two-slot frame grants it 50%. It does not mean the radio cannot exchange any SF12 packets.

The deployable limits are much lower than several airtime-only numbers:

- The shipped SF7 build supports four assigned nodes because `NUM_SLOTS=5`; expanding it currently trips retry-timing assumptions at a sixth total slot and eventually encounters fixed tracker/storage limits.
- The proposed SF12 build also targets four nodes. Although its reduced-load airtime-only bound is five, five 5-second node/base shares create a 30-second frame and only one normal long-frame opportunity per node every 30 seconds. The proposed BUNDLE-plus-STATUS production rate is about one packet every 28.9 seconds, before markers or retries, so that geometry would accumulate backlog. Four is the provisional maximum for this profile and still requires soak validation.
- No supported SF8–SF11 profiles are defined by this plan. Their rows show the trade space for choosing a fallback SF after measurement; each selected SF still needs explicit slot, cadence, reliability, and hardware validation.

## Capacity consequences

### Current traffic is impossible at SF12

A continuously active node currently produces a 15-sample bundle every 11.25 seconds. One such SF12 bundle occupies 7.217 seconds, or about 64% of the channel before STATUS, acknowledgements, retries, sync, and commands. Four nodes would demand about 257% of a single channel for first-transmission bundles alone. No TDMA slot adjustment can make that offered load fit.

Reducing bundle size is useful for fitting a bounded slot and limiting how much data one corrupted packet loses, but it is not a capacity win by itself. Smaller bundles repeat the 27-byte reference/header overhead more often. Sampling and STATUS cadence must be reduced at the same time.

### Starting-profile load

With eight samples per bundle and a four-second sample period, each node produces a BUNDLE every 32 seconds. With STATUS every 300 seconds:

- Per-node packet production is about 0.0346 frames/s versus one normal transmission opportunity every 25 seconds, or 0.0400 frames/s. This leaves about 13.5% packet-rate headroom before markers, commands, and retransmissions.
- First-transmission uplink airtime is about 14.45% per continuously active node: 13.85% BUNDLE plus 0.60% STATUS.
- Four continuously active nodes use about 57.8% raw channel airtime for those uplinks. Base traffic, window markers, joins, commands, and retries are additional.
- The base owns 20% of scheduled time. It can fit several small fire-and-forget frames in a 4.8 s usable window, but must use an airtime-aware deadline gate and rotate/coalesce cumulative acknowledgements rather than trying to acknowledge every received packet immediately.

The profile has usable but not generous headroom. Synchronized sensor-triggered bursts or a poor link that invokes retries can still fill the eight-entry queues. If the four-node soak test shows pressure, reduce sample/STATUS cadence before making slots still longer. If only one node is used for a range experiment, a separate two-slot profile can shorten turnaround, but results from that geometry do not prove four-node capacity.

## System effects and required design work

### 1. Modem configuration and compatibility

- Make spreading factor, bandwidth, coding rate, and preamble explicit shared network configuration rather than relying on the RadioHead default.
- Apply exactly the same modem profile to base, every node, and the passive sniffer. SF7 and SF12 devices on the same carrier are not a supported mixed deployment.
- Verify register readback or startup diagnostics so a partially flashed fleet is obvious.
- Keep the trial at CR 4/5. A later CR 4/8 or narrower-bandwidth experiment is a separate change with a new airtime and compliance analysis.
- Preserve an SF7 build/profile for immediate rollback.

### 2. Bundle sizing without a wire-format fork

- Keep `BinaryPacket::kBundleMaxDeltas=14` and the Python decoder's maximum of 14 as the wire-format ceiling. Existing decoders already accept a smaller runtime `delta_count`; no packet-layout change is needed.
- Introduce an operational bundle target separate from the protocol maximum and set the SF12 profile to seven deltas/eight samples.
- Make duty-cycle calculations derive from the operational sample count, not the wire maximum. Audit all arrays and tests so the 14-delta decode ceiling remains safe while transmit accumulation stops at seven.
- Replace packet-type-only TX budgets with a length- and modem-aware airtime calculation. A malformed or misconfigured 195-byte BUNDLE must be rejected or deferred rather than allowed to cross a five-second slot.

### 3. TDMA geometry and radio deadlines

- Start with five 5,000 ms slots and 100 ms guards, producing a 25-second frame and 4.8-second usable windows.
- Budget the 111-byte BUNDLE at its measured airtime plus software/radio margin, provisionally 4.6–4.7 seconds. Update the bounded local `waitPacketSent()` timeout; the current 340 ms bound would return while an SF12 transmission is still active and risks a later sleep or send aborting it.
- Use the same airtime calculator for node and base deadline admission. Never begin a transmission that cannot finish before the trailing guard.
- Limit the normal node schedule to one long transmission per slot. Permit an additional short frame only when the measured remaining budget proves it fits; a fixed “three sends” cap is not a timing guarantee.
- Re-characterize the guard and node RX wake-ahead. A 100 ms guard and 250–300 ms wake-ahead are reasonable initial test values, not conclusions.
- Update every hardcoded 900 ms/20 ms assumption in the edge sniffer service and browser visualization. Keep `DEFAULT_NUM_SLOTS=5` unless the deployed node count is intentionally changed.

### 4. Remove blocking link ACKs

Complete and broaden the deferred base-slot-overrun work as a prerequisite:

- Send `ACK_SUMMARY` fire-and-forget. Its cumulative bitmap is naturally repeated and can recover a lost summary.
- Send direct assignment `TIME_SYNC` fire-and-forget. A node continues requesting assignment until it receives one.
- Send `AWAKEN` fire-and-forget rather than waiting for a RadioHead ACK; successful receipt of the assignment sync is the application-level completion signal.
- Stop nodes from transmitting RadioHead ACKs for these paths. Commands already use application `CMD_ACK` and should remain that way.
- Do not enable `StrictLinkAck` in the SF12 profile. If any remote link-ACK path is intentionally retained for diagnostics, give it a separately derived timeout and prohibit it from scheduled slots unless its entire worst-case exchange fits.

This change is required for correctness, not merely throughput: a 250 ms timeout cannot observe an 827 ms SF12 ACK, and four blocking attempts cannot fit a five-second slot.

### 5. Reliability and queue timing

- Express ACK expectations, retry waits, sleeping-node silence, and pending age in frame units. With a 25-second frame, start retries after two frames (50 seconds), not after the current 9 seconds.
- Increase maximum pending age to at least 150 seconds so three total attempts can occur without expiring first. Confirm the exact value against Timed sleep duration and the desired failure-report latency.
- Re-evaluate the eight-entry pending window and TX queue using worst-case synchronized wake/burst tests. Do not enlarge them until SRAM cost and stale-data behavior are understood; reducing offered load is preferable.
- Allow the base to coalesce acknowledgements and rotate dirty nodes over multiple slot-0 windows. Set the sleeping-node silence threshold to about two frames rather than the current nine seconds.
- Increase `AWAKEN` repetition from five seconds to roughly one frame and add randomized jitter/backoff so several unassigned SF12 nodes do not transmit 1.319-second packets in lockstep.
- Start periodic broadcast TIME_SYNC near five frames (125 seconds). The 22-minute stale threshold may remain initially, but stale/unsynchronized permissive transmission is much more disruptive at SF12 and must be part of recovery testing.

### 6. Sensing and duty-cycle behavior

- Start continuously active sampling at four seconds. Update dashboard expected-sample calculations; the current browser assumption is 750 ms.
- Define SensorTriggered, Timed, and Hybrid active windows in whole operational bundles. At eight samples and four seconds, one bundle spans 32 seconds and two span 64 seconds.
- A Timed two-bundle window therefore needs about 64 seconds of sampling rather than 30. Start with a cycle of at least 150 seconds so 10-second warmup, sampling, up to one frame of final TX drain, and a meaningful standby all fit.
- Increase final TX drain from five seconds to at least one frame plus transmission margin, or implement a phase-aware close/drain design that avoids waiting a full frame. Measure the energy cost before selecting between them.
- Confirm the SPS30 and other sensor warmup/sample behavior at the slower cadence. Do not assume radio savings translate directly into battery savings when warmup-heavy sensors stay powered longer.

### 7. Base control and dynamic TX power

- Pin test nodes at 13 dBm STATIC so the existing controller does not reduce power during a “maximum range” trial.
- Before re-enabling DYNAMIC mode, change the SF-dependent demodulation floor used for SNR margin. The current `-7.5 dB` floor explicitly assumes SF7 and would make incorrect power decisions at SF12.
- Retune decision intervals, silence detection, command-ACK expectations, and UI warnings in frame units. Command delivery/acknowledgement can now take tens of seconds even when healthy.
- Do not raise the power ceiling beyond 13 dBm as part of this experiment. Higher PA settings, antenna gain, thermal/current limits, and radiated-power compliance require a separate review.

### 8. Watchdog, power, and failure exposure

- The same 111-byte payload takes about 22.75 times longer at SF12 than at SF7, so transmit energy per packet rises by approximately that factor at equal RF power. The slower packet cadence offsets part, not all, of this increase.
- Review the eight-second steady-state watchdog against a measured 4.432-second transmission plus sensor and loop latency. Feed immediately before and after legitimate bounded radio operations, never inside a potentially hung wait. Increase the timeout only if measurement proves necessary and retain useful hang detection.
- Long frames are more exposed to interference and oscillator drift. SF12 improves demodulation sensitivity but does not guarantee better packet delivery in a busy channel; one interferer can erase several seconds and up to eight samples.
- Expect higher end-to-end latency: a node gets a slot every 25 seconds, cumulative ACKs may arrive after one or more frames, retries start around 50 seconds, and commands may require a full frame each way.

### 9. Edge, diagnostics, and documentation

- Parameterize the sniffer's slot width and guard in both Python and JavaScript; ensure its radio firmware also uses SF12.
- Update expected sample cadence, stale/timeout labels, command progress, and range-test exports so healthy 25–150 second events are not shown as failures.
- Log the active modem tuple, slot geometry, operational bundle cap, and sample cadence at node/base startup and in captured test metadata.
- Update current architecture docs only when behavior ships. At that point, maintain reciprocal `docs:`/`source_refs`, update the packet/bandwidth tables, and run both documentation validators plus Python compile validation.

## Range gain and limitations

SF12 lowers the required demodulation SNR substantially relative to SF7; the theoretical link-budget improvement is on the order of the low teens in dB. That can be a large range improvement in clear line of sight, but distance does not scale one-for-one with dB. Terrain, foliage, antenna height/orientation, feedline loss, enclosure detuning, Fresnel-zone obstruction, local interference, and receiver noise often dominate.

The trial must compare SF7 and SF12 with the same boards, antennas, frequency, coding rate, bandwidth, power, mounting, weather, and route. Antenna placement and height should be optimized before attributing a result to spreading factor.

Long single-channel transmissions also require a deployment-specific regulatory review. Do not infer compliance from “915 MHz” or module capability. Confirm jurisdiction, equipment authorization, antenna/EIRP, channel-use and dwell/occupancy rules before an outdoor or production rollout.

## Implementation sequence

1. **Freeze success criteria.** Record the target terrain/range, required delivered-sample ratio, maximum acceptable latency, supported node count, desired battery life, and applicable radio rules. Capture an SF7 baseline on the same route.
2. **Add a separate SF12 profile.** Centralize modem and timing parameters, retain SF7 rollback, add startup identity/readback, and update host-side calculations/tests. Do not change deployed defaults yet.
3. **Make transmission deadline-safe.** Add actual-length airtime budgets, update local TX completion bounds, remove scheduled link ACK waits, add base and node deadline gates, and convert timers to frame-derived values.
4. **Apply the reduced-load profile.** Use seven deltas, four-second sampling, five-minute STATUS, whole-bundle duty windows, revised drain time, and static 13 dBm. Update edge/sniffer timing and UI expectations.
5. **One-node bench test.** Verify measured airtime for every packet class, slot containment, receiver wake, join/rejoin, TIME_SYNC, ACK summaries, commands, reset, queue behavior, watchdog margin, and current draw. Deliberately lose packets and ACK summaries.
6. **Controlled A/B range test.** At fixed checkpoints run SF7 then SF12, collecting sent/received/duplicate/missing sequence counts, first-try and final delivery, RSSI, SNR, retries, join time, command latency, queue/pending drops, resets, and energy. Repeat line-of-sight and obstructed tests.
7. **Four-node worst-case soak.** Run continuously active nodes and synchronized trigger/wake bursts for at least 24 hours. Include a sleeping node, simultaneous reboot/join, base restart, lost sync, interference, and the farthest intended link. Confirm no slot overlap or unbounded backlog.
8. **Tune downward from SF12 if needed.** If SF12 range is valuable but capacity, latency, or energy fails, test SF11 and SF10 with the same method. Choose the lowest SF that meets the range/reliability target; the highest SF is not automatically the best system setting.
9. **Deploy atomically or roll back.** Flash the base, nodes, and sniffer in a controlled maintenance window; mixed profiles cannot communicate. Keep known-good SF7 binaries/configuration and the recovery procedure onsite.

## Acceptance gates

Do not adopt SF12 beyond the experiment until all of these are evidenced:

- Every measured transmission finishes inside its assigned guarded slot with stated margin; no base or node send blocks across the next slot.
- The test fleet joins and recovers from simultaneous cold boot without persistent AWAKEN collisions.
- At the declared node count and sensing profile, queues and pending windows remain bounded with no steady-state drop trend, including STATUS and window markers.
- The target delivered-sample ratio and latency are met at the target range in both representative line-of-sight and obstructed conditions.
- Watchdog resets, TX-completion timeouts, missed sync, and command timeouts do not increase unexpectedly.
- Measured energy supports the deployment's battery target.
- The selected frequency/power/antenna/channel-use profile has passed the required regulatory review.
- All radios expose the same modem/TDMA profile, current docs match the shipped implementation, and the SF7 rollback has been exercised.

## Explicitly out of scope for the first trial

- Changing coding rate, bandwidth, preamble length, carrier frequency, or the 13 dBm power ceiling.
- Supporting mixed spreading factors in one TDMA frame.
- Changing the binary packet layout or reducing the decoder's 14-delta compatibility ceiling.
- Adding frequency hopping, CAD, mesh, or LoRaWAN.
- Claiming a range multiplier before controlled field measurements.
