# Bundle Timestamp Fix

## Background

When the Jetson receives a `PKT_BUNDLE`, the Python decoder (`packet.py:decode_bundle`)
expands it into one reference row plus up to 14 delta rows and writes each to the telemetry
CSV. Two separate bugs cause every row from the same bundle to carry the same timestamp,
making per-sample timing unrecoverable in analysis.

Observed symptom in the CSV (all five rows from bundle seq=15, node 3):

```
2026-06-17T00:31:54.863,telemetry,3,15,140838,140838,23,0.0,25.83,21.73,...
2026-06-17T00:31:54.863,telemetry,3,15,140838,140838,23,0.0,26.43,20.53,...
2026-06-17T00:31:54.863,telemetry,3,15,140838,140838,23,0.0,27.63,19.33,...
2026-06-17T00:31:54.863,telemetry,3,15,140838,140838,23,0.0,28.83,18.73,...
```

Sensor values (temperature, humidity) differ across rows, confirming the delta values
themselves are decoded correctly. The time axis is wrong.

---

## Bug 1 — `timestamp` column: same wall-clock time for all rows in a bundle

**Root cause:** `uart_receiver.py` lines 111–113.

```python
now_ts = datetime.datetime.utcnow().isoformat(timespec="milliseconds")
for pkt in packets:
    pkt["timestamp"] = now_ts
```

The entire bundle arrives as one UART frame and is decoded in a single `push_byte()` call.
One wall-clock instant is captured, then stamped on every decoded packet — reference and
all deltas alike. The Jetson has no independent way to know when each sample was physically
taken, but it does not need one: `session_time_ms` already encodes that information.

**Fix:** Reconstruct per-row wall-clock time from `session_time_ms`.

Each decoded packet already carries `session_time_ms`, which is milliseconds since the
Jetson session started (`session_start` in `ingest_service.py`). The correct wall-clock
time for each row is:

```
timestamp = utcfromtimestamp(session_start + session_time_ms / 1000.0)
```

This requires threading `session_start` from `ingest_service.py` into `FrameReceiver`
so it is available at the point where timestamps are stamped.

Note: this fix is only meaningful after Bug 2 is resolved, because until `session_time_ms`
is correct, reconstructing wall-clock time from it just propagates the same wrong value.

---

## Bug 2 — `session_time_ms` column: same value for all delta rows in a bundle

### Why it's wrong

Delta rows get their `session_time_ms` from the expression `session_time + dt_ms` in
`decode_bundle`, where `dt_ms = dt_ticks_250ms * 250`. If `dt_ticks_250ms` is zero for
all deltas, every row reconstructs to `session_time` — the reference frame's value.

### Design overflow in `dt_ticks_250ms`

`PacketHandler::makeDelta` (`PacketHandler.cpp`) stores the time delta as:

```cpp
const uint32_t dtMs = static_cast<uint32_t>(s.session_time - ref.session_time);
uint32_t dtTicks = (dtMs + 125u) / 250u;
if (dtTicks > 255u) { dtTicks = 255u; ... }   // CLAMPED
d.dt_ticks_250ms = static_cast<uint8_t>(dtTicks);
```

Each delta's `dt_ticks_250ms` is the cumulative offset **from the reference frame**,
not from the previous delta. With `kBundleMaxDeltas = 14` and `kContinuousSamplePeriodMs
= 20000 ms`, delta[13] would need to encode 280,000 ms. The field is `uint8_t`, so the
maximum representable offset is `255 × 250 ms = 63,750 ms`. Delta[3] onward are clamped
(all reconstruct to `ref + 63,750 ms` instead of `ref + 80,000 ms`, `ref + 100,000 ms`,
etc.). The design cannot represent more than ~63 seconds of cumulative offset.

### Firmware session-clock issue

Even for delta[0] through delta[2] — which should be within the representable range at
ticks 80, 160, and 240 — the CSV shows `session_time_ms = ref` (i.e., ticks = 0). This
means `s.session_time == ref.session_time` in the firmware for all 15 snapshots in the
bundle. The session clock (`TdmaClock::sessionNowMs()`) is returning the same value for
every call to `buildSnapshot()`.

The most likely cause: `TdmaClock::applySync()` is being called repeatedly during bundle
accumulation, resetting `_syncLocalMs` to the current `millis()` each time. After a
reset, `sessionNowMs() = _syncSessionMs + (millis() - _syncLocalMs) ≈ _syncSessionMs`
for the next few milliseconds, and if all samples are built within that window, they all
carry the same `session_time`. The ingest service sends a TIME_SYNC on every AWAKEN
packet. If the node re-sends AWAKENs for any reason during sampling, the clock anchor
would be reset.

This must be confirmed via firmware debug logs before the encoding fix below is assumed
to fully solve the problem.

---

## Fix Plan

### Phase 1 — Diagnose the firmware session-clock issue

Add debug logging in `PacketHandler::makeDelta` to surface the actual values reaching
the encoder:

```cpp
LOG_DEBUG("packet",
    "delta_dt ref_session_ms=%lu sample_session_ms=%lu dtMs=%lu dtTicks=%u",
    (unsigned long)ref.session_time,
    (unsigned long)s.session_time,
    (unsigned long)dtMs,
    (unsigned int)dtTicks);
```

Also add a log line in `buildSnapshot()` immediately after `sessionNowMs()` is called:

```cpp
LOG_DEBUG("app", "snapshot_session_ms=%lu", (unsigned long)snap.sessionTimeMs);
```

Run both node builds with `LOG_LEVEL=DEBUG` and capture the output during bundle
accumulation. Look for whether consecutive snapshot lines show the same or advancing
`session_ms` values, and whether `delta_dt` lines show `dtMs > 0`.

If `snapshot_session_ms` repeats, the session clock is not advancing and the root cause
is the `applySync()` call pattern. If it advances but `dtMs = 0`, there is a struct
copy or quantize bug elsewhere.

**Files:** `platformio/src/radio/PacketHandler.cpp`, `platformio/src/app/SmartFiresNodeApp.cpp`

---

### Phase 2 — Fix the encoding design: change to cumulative-from-previous

Regardless of the clock diagnosis outcome, the current cumulative-from-reference design
overflows for any bundle spanning more than 63.75 seconds. The fix is to change
`dt_ticks_250ms` to encode **ticks since the previous sample** rather than ticks since
the reference frame. With 20-second sample periods, each delta then encodes exactly
80 ticks — well within `uint8_t`. The wire format (struct layout and byte count) does
not change.

#### Firmware changes

**`platformio/include/radio/PacketHandler.h`**

Add `_prevSample` to bundle state:

```cpp
BinaryPacket::FullStatePayload _prevSample = {};  // time baseline for each delta
```

Update `makeDelta` signature to accept a `prev` argument in addition to `ref`:

```cpp
static BinaryPacket::DeltaPayload makeDelta(
    const BinaryPacket::FullStatePayload &ref,   // sensor-value baseline (unchanged)
    const BinaryPacket::FullStatePayload &prev,  // time baseline (previous sample)
    const BinaryPacket::FullStatePayload &sample);
```

**`platformio/src/radio/PacketHandler.cpp`**

In `makeDelta`, compute `dtMs` from `prev` rather than `ref`:

```cpp
const uint32_t dtMs = static_cast<uint32_t>(s.session_time - prev.session_time);
```

All sensor-value deltas remain relative to `ref` (unchanged). Only the time field uses
`prev`.

In `push()`, initialise `_prevSample` on the reference push and advance it on each
delta push:

```cpp
if (!_hasRef) {
    _ref = sample;
    _prevSample = sample;   // ← add
    _hasRef = true;
    ...
    return false;
}
_deltas[_deltaCount++] = makeDelta(_ref, _prevSample, sample);
_prevSample = sample;       // ← add: advance to current sample
```

In `resetBundleState()`, zero `_prevSample` alongside `_ref`:

```cpp
memset(&_prevSample, 0, sizeof(_prevSample));
```

#### Python decoder changes

**`edge/edge-receiver/src/smartfires_edge/packet.py` — `decode_bundle`**

Change the delta loop to accumulate time forward from the previous row rather than adding
each delta's offset to the fixed reference time:

```python
# Before the delta loop:
current_session_time = session_time   # start at reference frame's time

for _ in range(delta_count):
    (dt_ticks_250ms, ...) = struct.unpack_from(DELTA_FMT, raw_lora_payload, offset)
    offset += DELTA_SIZE

    dt_ms = dt_ticks_250ms * 250
    current_session_time += dt_ms     # accumulate forward

    results.append(
        _full_state_fields(
            node_id, seq,
            current_session_time,     # use accumulated time, not session_time + dt_ms
            ...
        )
    )
```

**Compatibility note:** firmware and Python must be updated together. Old firmware
(cumulative-from-reference) with new Python (accumulate-forward) will produce wrong
times in the opposite direction. Coordinate the deploy.

**Files:** `platformio/src/radio/PacketHandler.cpp`, `platformio/include/radio/PacketHandler.h`,
`edge/edge-receiver/src/smartfires_edge/packet.py`

---

### Phase 3 — Fix the `timestamp` column

With `session_time_ms` now correct, reconstruct per-row wall-clock timestamps from it.

**`edge/edge-receiver/src/smartfires_edge/uart_receiver.py`**

Add `session_start` as a constructor parameter on `FrameReceiver` and store it:

```python
class FrameReceiver:
    def __init__(self, session_start: float) -> None:
        self._session_start = session_start
        ...
```

Replace the single-stamp timestamp logic in `push_byte()`:

```python
# Remove:
now_ts = datetime.datetime.utcnow().isoformat(timespec="milliseconds")
for pkt in packets:
    pkt["timestamp"] = now_ts

# Replace with:
for pkt in packets:
    t = self._session_start + pkt["session_time_ms"] / 1000.0
    pkt["timestamp"] = datetime.datetime.utcfromtimestamp(t).isoformat(
        timespec="milliseconds"
    )
```

Update `iter_packets` to accept and forward `session_start`:

```python
def iter_packets(port: str, baud: int, session_start: float):
    receiver = FrameReceiver(session_start)
    ...
```

**`edge/edge-receiver/src/smartfires_edge/ingest_service.py`**

Pass `session_start` to `iter_packets`:

```python
for event, receiver, ser in iter_packets(cfg.port, cfg.baud, session_start):
```

**Files:** `edge/edge-receiver/src/smartfires_edge/uart_receiver.py`,
`edge/edge-receiver/src/smartfires_edge/ingest_service.py`

---

## Implementation Order

| Step | Description | Files | Prerequisite |
|---|---|---|---|
| 1 | Add `snapshot_session_ms` and `delta_dt` debug log lines | `SmartFiresNodeApp.cpp`, `PacketHandler.cpp` | None |
| 2 | Run debug build; confirm whether session clock advances across snapshots | — | Step 1 |
| 3 | Fix `_prevSample` tracking and `makeDelta` to use `prev` for time | `PacketHandler.h`, `PacketHandler.cpp` | Step 2 diagnosis |
| 4 | Fix `decode_bundle` to accumulate time forward | `packet.py` | Must ship with Step 3 |
| 5 | Pass `session_start` to `FrameReceiver`; reconstruct per-row `timestamp` | `uart_receiver.py`, `ingest_service.py` | Step 4 |

Steps 3 and 4 must be deployed together (firmware flash + Python update on Jetson).

---

## Files Affected (Summary)

### Firmware

- `platformio/src/radio/PacketHandler.cpp` — `makeDelta` takes `prev`; `push()` maintains `_prevSample`; debug logging
- `platformio/include/radio/PacketHandler.h` — add `_prevSample` field; update `makeDelta` signature

### Python / edge

- `edge/edge-receiver/src/smartfires_edge/packet.py` — `decode_bundle` accumulates time from previous row
- `edge/edge-receiver/src/smartfires_edge/uart_receiver.py` — `FrameReceiver` accepts `session_start`; per-row timestamp reconstruction
- `edge/edge-receiver/src/smartfires_edge/ingest_service.py` — passes `session_start` to `iter_packets`

### Unchanged

- `BinaryPacket.h` wire format — `DeltaPayload` struct layout and byte count do not change
- All other Python edge modules
- `csv_logger.py` — column schema unchanged; `timestamp` now carries a different (correct) value
