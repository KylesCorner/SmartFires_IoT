# Field Session Summary — 2026-10-16

## Session identity and configuration

- Jetson session directory: `/mnt/nvme_drive/data/2026-10-16_172027`
- Session ID: `0x092E4D2A`
- Session start: 2026-10-16 17:20:27 UTC
- Base serial device: `/dev/smartfires-base` at 115200 baud
- Registered node: node 2, UID hash `0xfb30178f`
- Network profile: SF12, 250 kHz bandwidth, coding rate 4/5
- TDMA geometry: 3 slots, 5,000 ms per slot, 100 ms guard
- Status interval: 15,000 ms
- Node transmit configuration observed in STATUS: 20 dBm, STATIC
- Base position used for range calculations: 46.865120, -113.982310

## Result and farthest packet received by the base

The farthest valid-GPS packet recorded through the base was **1,055.9 m** from the base (**0.656 mi / 3,464 ft**).

| Field | Value |
|---|---|
| Timestamp | 2026-10-16 19:15:34.873 UTC |
| Packet | STATUS |
| Node | 2 |
| Sequence | 137 |
| Position | 46.8617408, -113.9693312 |
| Distance | 1,055.9 m / 0.656 mi / 3,464 ft |
| Battery | 3,931 mV / 73% |
| Transmit power | 20 dBm STATIC |
| Retransmit total | 114 |
| Failed-send total | 0 |
| Stored RSSI | 124 |
| Interpreted RSSI | approximately -132 dBm |

The positive stored RSSI is an integer-width display/storage defect: values below -128 dBm wrap into positive `int8_t` values. `124` therefore corresponds to approximately -132 dBm. This packet was also the final node packet received by the base during the session.

## Packet summary

- `telemetry.csv` contains 212 records: 171 STATUS, 40 telemetry, and 1 AWAKEN.
- 168 records have valid GPS positions.
- Node 2 packet-loss state at the end of the session:
  - 111 unique packets received
  - 27 packets inferred missing
  - 61 duplicates
  - 19.57% inferred loss (`27 / (111 + 27)`)
  - 172 CRC-valid packets
  - 0 CRC failures and 1 length failure at the session level
- First recorded packet: 2026-10-16 17:20:39.881 UTC.
- Last packet received through the base: 2026-10-16 19:15:34.873 UTC.

## What happened

The session initially produced usable status and telemetry data. After a base reset at approximately 19:03 UTC, the base initialized successfully and received node traffic for roughly 12 minutes. Reception then stopped after 19:15:34 UTC even though the base MCU continued emitting health records and network-profile announcements over USB.

The independent sniffer continued receiving node traffic after the base stopped. Immediately before the later service restart, its cumulative counters showed 272 node packets with an average RSSI of approximately -95 dBm. Its last node observation was 19:18:19 UTC, nearly three minutes after the base's final node reception. The sniffer had observed 98 base packets with an average RSSI of approximately -44.5 dBm, but its last base observation was 19:15:42 UTC.

The base reported a successful periodic TIME_SYNC send at 19:16:27 UTC, but the sniffer did not observe a base packet after 19:15:42 UTC. This is important: the base firmware considered the packet queued successfully while the independent RF observer did not see it.

The base health stream remained active while RF reception was absent. It reported `rx_fail=0`; its receive counter simply stopped advancing and `last_rx_ms_ago` increased. This distinguishes the event from a Jetson ingest failure during that interval and suggests that the base radio stopped delivering packets to the firmware.

The base was externally reset/re-enumerated at approximately 19:03, 19:18, and 19:20 UTC. The recorded reset cause was `0x10` with `wdt=0`, so these were external/USB resets rather than base watchdog resets. The 19:03 reset restored reception temporarily; the later resets initialized the base radio successfully but did not restore node reception, by which time the node was also repeatedly resetting.

The Jetson kernel recorded repeated USB disconnect/re-enumeration events for both `/dev/ttyACM0` (base) and `/dev/ttyACM1` (sniffer). At 19:21:28 UTC the sniffer USB link disconnected with Tegra XUSB transfer errors. The edge service's sniffer reader then exited with a fatal serial error and did not automatically reopen the re-enumerated device. Restarting `smartfires-edge` reopened the sniffer and restored TDMA display updates.

The dashboard time-sync command was accepted by the Jetson and delivered over UART, but the deployed base firmware logged `action=cache_only_not_forwarded`. The Jetson had the new dashboard/API implementation, while the corresponding base firmware had not been flashed. A normal periodic TIME_SYNC was visible after the edge-service restart because the sniffer reader was working again.

Near the end of testing, the node began repeatedly resetting and stopped producing observable AWAKEN packets. A full battery disconnect provided a cold boot but did not remove the underlying fault. Field inspection identified a suspected bad solder point at the SPS/TPS connection. This is the leading node-specific hardware issue to inspect. A power or connection fault can recur immediately after a clean reset, especially when the node initializes its sensors or begins a 20 dBm radio transmission.

## Base versus sniffer observations

The sniffer was the more reliable RF observer during this session:

- It continued hearing node packets after the base receive counter froze.
- The base's node signal at maximum range was approximately -140 to -129 dBm with SNR around -21 to -10 dB, while the sniffer's cumulative node average was about -95 dBm.
- If the base and sniffer were colocated, this 30–45 dB difference is too large to attribute to ordinary receiver variation.
- A different antenna was installed on the base during the test. Antenna frequency rating, connector seating, cable loss, ground-plane/orientation differences, and the radio module's RF connection should all be checked.
- Antenna loss explains the much weaker base reception, but it does not fully explain the apparent simultaneous loss of base RX and observable TX. A radio state/interrupt lockup remains a serious hypothesis.
- The fact that resetting the base restored operation for roughly 12 minutes points toward recoverable radio state, interrupt, SPI, or power behavior rather than a persistent TDMA-profile mismatch.

## Working hypotheses

### Base station

1. **Radio state or TX-complete interrupt lockup.** The base continued running but stopped receiving, and it later reported a successful send that the sniffer did not observe. Resetting the base temporarily restored operation.
2. **Poor base antenna/RF path.** The changed antenna coincided with a large link-budget disadvantage relative to the sniffer and could make the base far more sensitive to movement or marginal conditions.
3. **Base USB/power instability.** Multiple physical USB disconnects and Tegra XUSB errors occurred. These explain some resets and dashboard interruptions, though not the interval where the base MCU was clearly alive but RF reception was frozen.

### Node

1. **Suspected bad SPS/TPS solder point or related power connection.** This is the primary physical finding from the field inspection.
2. **Power sag during startup or 20 dBm transmission.** A cold reset will repeat immediately if the supply path drops under load.
3. **Radio TX hang.** Repeated AWAKEN transmissions while waiting for synchronization exercise the radio TX path, and a missed TX-complete interrupt can lead to a watchdog reset.
4. **Sensor initialization failure.** A bad SPS/TPS connection can make application initialization fail; the firmware intentionally allows the watchdog to reboot when initialization cannot complete.

## Takeaways

- Treat the sniffer as an independent RF truth source, separate from base-to-Jetson serial health and dashboard rendering.
- Record base reception and sniffer reception concurrently with synchronized clocks; preserve both streams even if the dashboard restarts.
- A dashboard `202 Accepted` response confirms command queuing, not an over-air transmission.
- Distinguish a base RF freeze from a dead base MCU: continuing health/profile logs with a frozen RX counter indicate the MCU is alive.
- Serial readers need automatic reopen/recovery after USB disconnect and device re-enumeration.
- Correct the signed RSSI representation so very weak readings do not appear as positive values.
- Use a known-good, frequency-matched antenna and connector on the base before the next range comparison.
- Capture node reset cause and hang zone over serial; the absence of an AWAKEN packet prevents those diagnostics from reaching the Jetson over RF.

## Next-session plan

- Test with **two nodes**.
- Use a **Raspberry Pi to save continuous raw serial output** from the node under investigation.
- Kyle will focus on the node hardware and Raspberry Pi capture, including the suspected SPS/TPS solder point.
- Investigate why resetting the base restored connectivity temporarily.
- Compare, packet by packet, what the sniffer receives versus what the base receives.
- Record the onset characteristics of any failure: last base RX, last sniffer RX, last base TX observed by the sniffer, base health counters, USB events, node reset cause, and node hang zone.
- Start with the base and sniffer colocated and using known-good comparable antennas. Do not change antennas without recording the exact time and hardware combination.
- Power the node from a known-good source for an A/B test against battery power.
- Begin Raspberry Pi serial capture before applying node power so the first cold-boot reset cause and final line before every reset are retained.

## Recommended capture fields for the next session

- UTC timestamp on every Raspberry Pi serial line.
- Node `reset_cause`, `hang_zone`, and `boot_count`.
- Last boot milestone reached: sensor initialization, radio initialization, or AWAKEN transmission.
- Base `health_rx`, `health_link`, ACK-summary transmissions, TIME_SYNC transmissions, and any TX timeout.
- Sniffer packet type, sender, sequence, RSSI/SNR, slot, and timestamp.
- Jetson kernel USB connect/disconnect messages.
- Antenna, cable, power source, node position, and base position for every test segment.

## Saved evidence

The session directory contains:

- `session.json`
- `telemetry.csv`
- `status.jsonl`
- `packet_loss_state.json`
- `ingest_log.log`
- `base_debug.log`

The live and base-debug logs were explicitly flushed to the Jetson NVMe before this summary was written.
