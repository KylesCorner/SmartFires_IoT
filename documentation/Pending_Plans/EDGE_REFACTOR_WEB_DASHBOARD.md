# Edge Refactor — Web Dashboard, Session Metadata, and TDMA Sniffer

## Background

The Jetson edge side currently has two useful but disconnected interactive modes:

- `smartfires-edge receive` — headless UART ingest loop. Prints structured lines to stdout,
  logs telemetry to a rotating daily CSV. No interactivity.
- `smartfires-edge web` — the same ingest loop running in a background thread, plus a
  FastAPI/uvicorn server serving a partially-built web dashboard (Leaflet map, Chart.js
  charts, `/api/nodes`, `/api/telemetry/history`, etc.). Already exists but lacks live
  log streaming, per-node filtering, and command input.
- `smartfires-edge cli` — a curses TUI for calibrate/reset commands. Kept for SSH sessions
  without a browser.

Three gaps drive this refactor:

1. **Session metadata** — the node serial → node_id mapping lives only in `session.json`
   (SessionManager). Nothing adjacent to the telemetry CSV tells a post-processing script
   which uid_hash corresponds to which node_id, or what port/session_id the data came from.

2. **Live log stream + per-node filter** — the browser has no way to see what `receive`
   is printing. Operators watch stdout in a terminal; there is no per-node view.

3. **TDMA sniffer** — a second Feather M0 running the passive sniffer firmware
   (`feather_m0_lora_sniffer`) will be connected to the Jetson via USB serial. The
   firmware outputs NDJSON (one object per received LoRa packet) including raw timing,
   RSSI, SNR, RadioHead headers, and `payload_hex`. Python is responsible for decoding
   packets and computing TDMA slot positions. A browser tab should visualize the TDMA
   timing swim-lane and per-node RF statistics.

**Goal:** Make `smartfires-edge web` the primary interactive command for field use.
`receive` stays as a headless/automated logging mode and gains only the metadata file.

---

## Decisions

| Question | Decision |
|---|---|
| Primary interactive mode | Browser-first (`smartfires-edge web`) |
| Per-node filter UI | Tab bar: **All \| Node 2 \| Node 3 \| ...** — tabs appear dynamically |
| Terminal vs. browser | Live log streamed to browser via WebSocket; terminal stays as a dumb pipe |
| Session metadata | Live-updating JSON written adjacent to daily telemetry CSV |
| Sniffer integration | `web` only, via optional `--sniffer-port`; no sniffer logging in headless `receive` |
| TDMA timeline window | 60-second sliding window |
| Command input | Stub now (`POST /api/command` returns `{"status": "queued"}`); wired to UART when firmware commands are ready |
| Subcommand shape | Keep `receive` (headless) and `web` (interactive) as separate commands |

---

## Phase 1 — Session Metadata File

**Status: Not started**

### New file: `session_meta.py`

A `SessionMetaLogger` class that writes and atomically updates a session manifest
alongside the daily telemetry CSV.

**Output path:** `data_dir/telemetry/session-YYYY-MM-DD.json`

**Contents:**
```json
{
  "session_id": "0xABCD1234",
  "started_at": "2026-06-17T10:30:00.000Z",
  "port": "/dev/ttyTHS1",
  "baud": 115200,
  "node_registry": {
    "2": { "uid_hash": "0x12345678", "first_seen": "2026-06-17T10:30:15Z" },
    "3": { "uid_hash": "0x87654321", "first_seen": "2026-06-17T10:31:00Z" }
  }
}
```

Written atomically on startup. Atomic-overwritten on each new AWAKEN that adds a
node to the registry. Uses the same `atomic_write_json` helper from `state_store.py`.

The date in the filename matches the telemetry CSV filename for the same session, so
a post-processing script can find the metadata for any given CSV by substituting
`telemetry-` → `session-`.

### Modified: `ingest_service.py`

Instantiate `SessionMetaLogger` at the top of `run_receive`. Call
`meta.on_awaken(node_id, uid_hash)` in the AWAKEN handler. Pass `session_id`,
`session_start`, `cfg.port`, and `cfg.baud` to the constructor.

Both `receive` and `web` call `run_receive`, so both get the metadata file automatically.

---

## Phase 2 — Log Streaming Architecture

**Status: Not started**

The current `ingest_service.py` uses raw `print()` calls. For the web mode, those same
lines must reach the browser over a WebSocket, tagged with node_id so the client can
filter by tab.

### Modified: `ingest_service.py`

Replace all `print(msg)` calls with `log_fn(msg, node_id=None)` where `log_fn` is a
new optional parameter to `run_receive`:

```python
def run_receive(
    cfg: IngestConfig,
    live_state: LiveState | None = None,
    log_fn: Callable[[str, int | None], None] | None = None,
) -> int:
    if log_fn is None:
        log_fn = lambda msg, node_id=None: print(msg)
    ...
```

In `receive` mode: caller passes nothing → `log_fn` defaults to `print`. Zero behavior
change for headless use.

In `web` mode: caller passes `live_state.push_log` as `log_fn`.

### Modified: `live_state.py`

Add a thread-safe log ring buffer:

```python
self._log_ring: deque[dict] = deque(maxlen=2000)
self._log_lock = threading.Lock()

def push_log(self, msg: str, node_id: int | None = None) -> None:
    with self._log_lock:
        self._log_ring.append({
            "t": datetime.utcnow().isoformat(timespec="milliseconds"),
            "msg": msg,
            "node_id": node_id,
        })

def drain_log(self, since_idx: int) -> tuple[list[dict], int]:
    with self._log_lock:
        items = list(self._log_ring)
    return items[since_idx:], len(items)
```

### Modified: `web/app.py`

Add a WebSocket endpoint:

```
GET /ws/log
```

The client connects once; the server streams log entries as newline-delimited JSON.
The client buffers entries and applies the active node-tab filter in JS (no re-request
per filter change).

### Modified: `web_service.py`

Pass `log_fn=live_state.push_log` when calling `run_receive`.

---

## Phase 3 — TDMA Sniffer Service

**Status: Not started**

### Hardware

A second Adafruit Feather M0 RFM95 running `feather_m0_lora_sniffer` firmware, connected
to the Jetson via USB serial (separate device from the base station's `/dev/ttyTHS1`).
Typical path: `/dev/ttyUSB0` or `/dev/ttyACM0`.

The sniffer firmware outputs NDJSON at 115200 baud. Each received LoRa packet emits one
`{"event":"rx", ...}` object with `t_ms`, `dt_ms`, `len`, `rssi`, `snr`, `rh_to`,
`rh_from`, `rh_id`, `rh_flags`, `payload_hex`. Startup events (`status`, `error`,
`config`) are also emitted.

The sniffer runs in promiscuous mode and hears everything on 915 MHz including
TIME_SYNC broadcasts (sent to `RH_BROADCAST_ADDRESS = 0xFF`).

### New file: `sniffer_service.py`

`run_sniffer(port, baud, num_slots, live_state)` — runs as a daemon thread inside
`web_service.run_web`.

Processing per `rx` event:
1. Parse `payload_hex` via `packet.py` decoders to get `pkt_type` and `node_id`.
2. On TIME_SYNC: record `(sniffer_t_ms, session_ms)` anchor pair. From this point,
   `session_ms_for(t_ms) = session_ms_anchor + (t_ms - t_ms_anchor)`.
3. Compute TDMA position if anchored:
   - `frame_period_ms = num_slots × 900`
   - `slot = (node_id - 1) % num_slots`
   - `slot_center_ms = slot × 900 + 450`
   - `frame_phase_ms = session_ms % frame_period_ms`
   - `jitter_ms = frame_phase_ms - slot_center_ms` (positive = late, negative = early)
   - `guard_violation = abs(jitter_ms) > (450 - 20)`
4. Push a structured event to `live_state.sniffer_ring`.

### Modified: `live_state.py`

Add `sniffer_ring: deque[dict]` (maxlen=5000) with `push_sniffer_event` / `drain_sniffer`
methods, and a per-node stats accumulator (packet count, RSSI sum, SNR sum, jitter
samples, guard violation count) with a `sniffer_stats_snapshot()` method.

### Modified: `config.py` and `main.py`

Add to the `web` subparser:
- `--sniffer-port` (str, default None — sniffer disabled if not given)
- `--sniffer-baud` (int, default 115200)
- `--num-slots` (int, default 2 — must match the `NUM_SLOTS` build flag on the node Feathers)

### Modified: `web_service.py`

If `cfg.sniffer_port` is set, start a daemon thread running `sniffer_service.run_sniffer`.

### Modified: `web/app.py`

Add:
- `GET /ws/sniffer` — WebSocket that streams `sniffer_ring` events as NDJSON.
- `GET /api/sniffer/stats` — returns `live_state.sniffer_stats_snapshot()`.

---

## Phase 4 — Browser UI

**Status: Not started**

### Main page (`index.html` / `main_page.js`) — log panel + node tabs

**Log panel:**
- A scrolling `<pre>` (or similar) element fed by `WS /ws/log`.
- Capped at 2000 lines client-side; older lines dropped.
- Each log entry has `node_id` metadata; only entries matching the active tab are rendered.

**Tab bar above the log panel:**
- Rendered as `All | Node 2 | Node 3 | ...`
- "All" is always present. Node tabs appear dynamically the first time a log entry with
  that `node_id` arrives.
- Active tab stored in JS state. No round-trips — filtering is client-side.

**Command input:**
- A text input + "Send" button below the log panel.
- On submit: `POST /api/command` with `{"command": "<text>"}`.
- Server logs the command and returns `{"status": "queued", "command": "<text>"}`.
- When firmware commands are implemented, this endpoint gets wired to the UART write path.

**New API endpoint in `web/app.py`:**
```
POST /api/command
Body: {"command": "reset node 2 soft"}
Response: {"status": "queued", "command": "reset node 2 soft"}
```

### New page: `sniffer.html` / `sniffer_page.js`

Linked from the existing nav bar.

**Panel 1 — Swim-lane timeline (Canvas):**
- X axis: wall-clock time, 60-second sliding window, scrolling continuously.
- Y axis: one horizontal lane per node (labeled "Node 2", "Node 3", etc.).
- Each received packet: a colored rectangle at the correct X position within its lane.
  - BUNDLE = blue
  - STATUS = green
  - AWAKEN = yellow
  - TIME_SYNC = grey (spans all lanes as a vertical line)
  - Unknown = red
- Vertical dashed lines at expected slot boundaries (derived from `num_slots` and
  `slot_width_ms = 900`).
- Grey shaded bands for guard periods (20 ms at each slot edge).
- Tooltip on hover: packet type, node_id, RSSI, SNR, jitter_ms.
- Disabled / "waiting for sniffer" state shown if `--sniffer-port` not configured.

**Panel 2 — Per-node stats table:**

| Node | Packets | Avg RSSI | Avg SNR | Jitter ±ms | Guard Violations |
|---|---|---|---|---|---|
| 2 | 142 | -67 dBm | 9.2 dB | ±3.1 | 0 |

Polled from `GET /api/sniffer/stats` every 5 seconds.

**Panel 3 — Sniffer packet log:**
- Same scrolling log style as the main page log panel.
- Shows decoded packet type + node_id + RSSI + SNR for each sniffer event.
- No node-tab filter needed here (all nodes shown; the timeline is the per-node view).

---

## Implementation Order

| Step | Description | Files |
|---|---|---|
| 1 | `SessionMetaLogger` + wire into `ingest_service.py` | `session_meta.py` (new), `ingest_service.py` |
| 2 | `log_fn` callback refactor in `ingest_service.py` | `ingest_service.py` |
| 3 | `live_state.py` log ring + `web_service.py` wiring | `live_state.py`, `web_service.py` |
| 4 | `/ws/log` WebSocket endpoint | `web/app.py` |
| 5 | Browser log panel + node tabs | `index.html`, `main_page.js` |
| 6 | `POST /api/command` stub | `web/app.py`, `index.html`, `main_page.js` |
| 7 | `sniffer_service.py` + `live_state` sniffer ring | `sniffer_service.py` (new), `live_state.py` |
| 8 | Sniffer config args + thread startup | `config.py`, `main.py`, `web_service.py` |
| 9 | `/ws/sniffer` + `/api/sniffer/stats` | `web/app.py` |
| 10 | `sniffer.html` swim-lane timeline + stats table + log | `sniffer.html` (new), `sniffer_page.js` (new) |

---

## Files Affected (Summary)

### New files
- `edge/edge-receiver/src/smartfires_edge/session_meta.py`
- `edge/edge-receiver/src/smartfires_edge/sniffer_service.py`
- `edge/edge-receiver/src/smartfires_edge/web/static/sniffer.html`
- `edge/edge-receiver/src/smartfires_edge/web/static/js/sniffer_page.js`

### Modified files
- `edge/edge-receiver/src/smartfires_edge/ingest_service.py`
- `edge/edge-receiver/src/smartfires_edge/live_state.py`
- `edge/edge-receiver/src/smartfires_edge/web_service.py`
- `edge/edge-receiver/src/smartfires_edge/config.py`
- `edge/edge-receiver/src/smartfires_edge/main.py`
- `edge/edge-receiver/src/smartfires_edge/web/app.py`
- `edge/edge-receiver/src/smartfires_edge/web/static/index.html`
- `edge/edge-receiver/src/smartfires_edge/web/static/js/main_page.js`

### Unchanged
- `smartfires-edge receive` behavior (gains only the metadata file)
- `smartfires-edge cli` curses subcommand
- Existing chart and map pages
- All firmware (`platformio/`)
