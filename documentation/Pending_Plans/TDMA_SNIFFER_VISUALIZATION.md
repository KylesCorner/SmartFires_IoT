# TDMA Sniffer — Firmware, Service, and Visualization

## Background

A second Adafruit Feather M0 RFM95 will be dedicated to passive LoRa monitoring. It
runs the `feather_m0_lora_sniffer` build environment, sits connected to the Jetson via
USB serial, and hears every packet on the 915 MHz channel — node telemetry, base station
TIME_SYNC broadcasts, and anything else — without participating in TDMA.

The goal is a browser tab in `smartfires-edge web` that makes TDMA timing and RF link
quality immediately visible: which nodes are transmitting in which slots, whether there
is drift or guard-band violations, and per-node RSSI/SNR trends.

This plan covers both sides: the firmware running on the sniffer Feather and the
Python service + web UI on the Jetson. Both should be reviewed and updated together.

---

## Current Firmware State (`main_lora_sniffer.cpp`)

The sniffer firmware already exists and is functional. It:

- Runs `RH_RF95` in promiscuous mode (receives all packets regardless of RadioHead
  destination address).
- Emits one NDJSON object per line over USB serial at 115200 baud for every received
  packet.
- Each `rx` event includes: `t_ms` (Feather `millis()`), `dt_ms` (inter-packet gap),
  `len`, `rssi`, `snr`, `rh_to`, `rh_from`, `rh_id`, `rh_flags`, `payload_hex`.
- Also emits `status`, `error`, `config`, and `rx_fail` events on startup and failure.
- A comment in the source explicitly states: **"Python owns all packet decoding, packet
  naming, TDMA classification, and dashboard filtering."**

### Things to review / potentially improve in the firmware

Before implementing the Python side, audit the firmware for:

1. **Modem config match** — the sniffer must use the same spreading factor, bandwidth,
   and coding rate as the deployed nodes. The current firmware has a commented-out
   `rf95.setModemConfig(RH_RF95::Bw125Cr45Sf128)` line. Verify whether the default
   modem config matches the node firmware's config, or whether that line needs to be
   uncommented and matched explicitly.

2. **SNR availability** — `lastSNR()` is guarded by `#if defined(RH_RF95_REG_PKT_SNR_VALUE)`.
   Confirm this is defined for the Feather M0 RFM95 target. If not, the `snr` field will
   always be `null` and the stats table will be less useful.

3. **Timestamp precision** — `t_ms` is `millis()` on the Feather (32-bit, wraps at ~49
   days). Adequate for TDMA slot timing (sub-second resolution needed). No change
   required, but document the wrap behaviour in the Python service.

4. **Output rate / serial buffer** — with 2 nodes at a 15-sample bundle rate (one bundle
   per ~15 s), the output rate is low. With more nodes or during burst testing, confirm
   the Serial TX buffer does not stall the `loop()`. If it does, consider non-blocking
   serial output.

---

## Python Service — `sniffer_service.py`

**New file.** Runs as a daemon thread inside `web_service.run_web` when
`--sniffer-port` is given.

### Configuration

New args on the `web` subcommand (in `config.py` / `main.py`):
- `--sniffer-port` (str, default `None` — sniffer disabled if not given)
- `--sniffer-baud` (int, default `115200`)
- `--num-slots` (int, default `2` — must match the `NUM_SLOTS` build flag baked into
  the node Feathers)

### Processing pipeline per `rx` event

1. Parse `payload_hex` bytes through `packet.py` decoders (`decode_awaken`,
   `decode_bundle`, `decode_status`, `decode_time_sync`) to get `pkt_type` and
   `node_id`. Unknown payloads are kept as raw hex.

2. **TIME_SYNC anchor:** when a TIME_SYNC is decoded, record the pair
   `(sniffer_t_ms, session_ms)`. All subsequent slot-relative timing uses:
   ```
   session_ms_for(t_ms) = session_ms_anchor + (t_ms - t_ms_anchor)
   ```
   The sniffer hears TIME_SYNC because it is broadcast to `RH_BROADCAST_ADDRESS = 0xFF`
   and the sniffer is in promiscuous mode.

3. **TDMA slot position** (only computed once anchored):
   ```
   frame_period_ms  = num_slots × 900
   slot             = (node_id - 1) % num_slots
   slot_center_ms   = slot × 900 + 450
   frame_phase_ms   = session_ms % frame_period_ms
   jitter_ms        = frame_phase_ms - slot_center_ms   # + = late, − = early
   guard_violation  = abs(jitter_ms) > (450 − 20)       # 20 ms guard band
   ```

4. Push a structured event dict to `live_state.sniffer_ring`:
   ```json
   {
     "wall_t": "2026-06-17T10:30:15.123Z",
     "sniffer_t_ms": 12345,
     "session_ms": 45678,
     "pkt_type": "BUNDLE",
     "node_id": 2,
     "rssi": -67,
     "snr": 9.2,
     "jitter_ms": 3.1,
     "guard_violation": false,
     "anchored": true,
     "payload_hex": "a504..."
   }
   ```

### `live_state.py` additions

- `sniffer_ring: deque[dict]` (maxlen 5000) with `push_sniffer_event` / `drain_sniffer`
  methods.
- Per-node stats accumulator: packet count, RSSI sum/count, SNR sum/count, jitter
  samples list (for std dev), guard violation count.
- `sniffer_stats_snapshot()` → dict keyed by node_id.

---

## Web API

New endpoints in `web/app.py`:

```
GET /ws/sniffer
```
WebSocket. Streams `sniffer_ring` events as NDJSON. Client connects once; server
pushes events as they arrive. Sends a `{"event": "not_configured"}` message and closes
if `--sniffer-port` was not given, so the browser can show a clear disabled state.

```
GET /api/sniffer/stats
```
Returns `live_state.sniffer_stats_snapshot()`. Polled by the browser every 5 seconds
to update the stats table.

---

## Browser UI — TDMA Tab (`sniffer.html` / `sniffer_page.js`)

Linked from the existing nav bar alongside the main dashboard and map pages.

### Panel 1 — Swim-lane timeline (Canvas)

- **X axis:** wall-clock time, 60-second sliding window, scrolling continuously left.
- **Y axis:** one horizontal lane per node (labeled "Node 2", "Node 3", etc.). Lanes
  appear dynamically as new node_ids are seen.
- **Packet blocks:** each received packet is a colored rectangle at its X position in
  the correct lane.
  - BUNDLE = blue
  - STATUS = green
  - AWAKEN = yellow
  - TIME_SYNC = grey vertical line spanning all lanes
  - Unknown = red
- **Slot boundary lines:** vertical dashed lines every `slot_width_ms = 900 ms` based
  on `num_slots`. Derived from the TIME_SYNC anchor.
- **Guard bands:** grey shaded 20 ms regions at each slot edge.
- **Tooltip on hover:** packet type, node_id, RSSI, SNR, jitter_ms.
- **Disabled state:** if the WebSocket sends `{"event": "not_configured"}`, show a grey
  panel with "Sniffer not connected — start with --sniffer-port".

### Panel 2 — Per-node stats table

Polled from `GET /api/sniffer/stats` every 5 seconds.

| Node | Packets | Avg RSSI | Avg SNR | Jitter ±ms | Guard Violations |
|---|---|---|---|---|---|
| 2 | 142 | -67 dBm | 9.2 dB | ±3.1 | 0 |
| 3 | 138 | -71 dBm | 7.8 dB | ±4.0 | 1 |

### Panel 3 — Sniffer packet log

Scrolling log of sniffer events (same style as the main dashboard log panel).
Shows decoded packet type + node_id + RSSI + SNR for each event. All nodes shown;
no per-node tab filter (the timeline is the per-node view).

---

## Implementation Order

| Step | Description | Files |
|---|---|---|
| 1 | Audit and update sniffer firmware | `platformio/src/main_lora_sniffer.cpp` |
| 2 | `sniffer_service.py` core (NDJSON reader, packet decode, TDMA math) | `sniffer_service.py` (new) |
| 3 | `live_state.py` sniffer ring + stats accumulator | `live_state.py` |
| 4 | Sniffer config args + thread startup in `web_service.py` | `config.py`, `main.py`, `web_service.py` |
| 5 | `/ws/sniffer` + `/api/sniffer/stats` endpoints | `web/app.py` |
| 6 | `sniffer.html` swim-lane Canvas timeline | `sniffer.html` (new), `sniffer_page.js` (new) |
| 7 | Per-node stats table + packet log panels | `sniffer.html`, `sniffer_page.js` |

---

## Files Affected (Summary)

### Firmware
- `platformio/src/main_lora_sniffer.cpp` — audit modem config, SNR availability, serial buffer

### New Python/web files
- `edge/edge-receiver/src/smartfires_edge/sniffer_service.py`
- `edge/edge-receiver/src/smartfires_edge/web/static/sniffer.html`
- `edge/edge-receiver/src/smartfires_edge/web/static/js/sniffer_page.js`

### Modified Python/web files
- `edge/edge-receiver/src/smartfires_edge/live_state.py`
- `edge/edge-receiver/src/smartfires_edge/config.py`
- `edge/edge-receiver/src/smartfires_edge/main.py`
- `edge/edge-receiver/src/smartfires_edge/web_service.py`
- `edge/edge-receiver/src/smartfires_edge/web/app.py`

### Prerequisite
This plan assumes `EDGE_REFACTOR_WEB_DASHBOARD.md` is complete first — specifically
the `live_state.py` log ring and the `/ws/log` WebSocket infrastructure, which the
sniffer tab reuses for its packet log panel.
