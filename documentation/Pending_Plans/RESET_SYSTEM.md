---
name: reset-system
description: Plan to wire up base-station and per-node CMD_RESET handling end-to-end, including Jetson auto-reset of the base on session start.
category: plan-pending
status: draft
related_docs:
  - tdma-protocol
  - packet-reliability
  - uart-jetson-bridge
---

# Reset System

## Background

Time sync diverges when components are reset independently:

1. **Jetson session restart** — new `session_id` is chosen; base and nodes eventually adopt it on the next periodic TIME_SYNC, but there is a delay window where `session_time` values diverge between the new Jetson session and running nodes.
2. **Base station LoRa radio entering a bad state** — radio hangs; no LoRa RX/TX until the RH_RF95 is reinitialized. No automatic recovery path exists today.
3. **Node power cycle or MCU reset** — node loses sync, heals via the AWAKEN → TIME_SYNC boot handshake. Already works correctly.
4. **Partial resets** (e.g., Jetson restarts but base stays up) — base continues broadcasting its own cached session clock until the next Jetson TIME_SYNC, causing nodes to briefly timestamp against a stale `session_id`.

Goals:
- Jetson explicitly resets the base station LoRa radio at the start of every new receive/web session.
- Jetson can reset individual nodes by node_id.
- Resets can be soft (re-enter AWAKEN loop, no MCU reboot) or hard (full `NVIC_SystemReset()`).

---

## Protocol Status — No Wire Changes Needed

All packet types and encoders already exist:

| Piece | Location | Status |
|---|---|---|
| `PKT_CMD_RESET (0x11)` enum | `BinaryPacket.h`, `packet.py` | Done |
| `CmdResetPayload(node_id, reset_type)` struct | `BinaryPacket.h`, `packet.py` | Done |
| `encode_cmd_reset_frame(node_id, reset_type, seq)` | `packet.py` | Done |
| `CMD_ACK (0x13)` round-trip | Both sides | Done |
| Base station UART decode + forward (`handleJetsonCommandPayload`) | `SmartFiresBaseApp.cpp` | Done (gaps below) |
| Node receive + ACK send (`handleIncomingCommands`) | `SmartFiresNodeApp.cpp` | Done (gaps below) |

**Convention for base self-reset**: `target_node_id = 0x00` in `CmdResetPayload` means "reset the base station itself" — the base intercepts and does not forward over LoRa. `target_node_id = N` (N > 0) is forwarded to node N. Node address 0 is already reserved (never assigned to a real node), so this is safe.

---

## Gaps

### Gap 1 — Base station: no self-reset path

**File**: `platformio/src/app/SmartFiresBaseApp.cpp`, `handleJetsonCommandPayload()` (~line 811)

The `PKT_CMD_RESET` branch calls `_radio.sendToWait(..., cmd.node_id)` unconditionally. When `cmd.node_id == 0` it tries to LoRa-transmit to address 0, which is meaningless. A `node_id == 0` branch must intercept before the forward:

```cpp
if (cmd.node_id == 0) {
    LOG_INFO("base", "uart_cmd_reset_self reset_type=%u seq=%u",
             static_cast<unsigned int>(cmd.reset_type),
             static_cast<unsigned int>(cmdHdr.seq));
    if (cmd.reset_type == 0x01) {
        NVIC_SystemReset();   // hard: full MCU reboot
    }
    // Soft: reinit radio, clear stale Jetson time, reset ACK trackers
    _hasJetsonTime = false;
    for (auto &t : _ackTrackers) { t = AckTracker{}; }
    _lastPeriodicTimeSyncMs = 0;  // force TIME_SYNC broadcast on next maybeSendPeriodicTimeSync() tick
    const bool ok = _radio.begin();
    LOG_INFO("base", "uart_cmd_reset_self_done radio_reinit=%s", ok ? "OK" : "FAIL");
    return true;
}
// ...existing sendToWait forward path...
```

`_nodeAssignments` is kept intact on soft reset — nodes don't need new IDs just because the radio was reinit'd. Only a hard reset (MCU reboot) clears the assignment table.

Setting `_lastPeriodicTimeSyncMs = 0` causes `maybeSendPeriodicTimeSync()` to broadcast a TIME_SYNC on the very next `update()` tick, so nodes resync quickly without waiting up to 10 minutes.

### Gap 2 — Node: CMD_RESET is acknowledged but not executed

**File**: `platformio/src/app/SmartFiresNodeApp.cpp`, `handleIncomingCommands()` (~line 426)

After `sendCmdAck(PKT_CMD_RESET, ...)` the code just `continue`s — no actual reset occurs. The following must be added immediately after the ACK send:

```cpp
sendCmdAck(BinaryPacket::PKT_CMD_RESET, kCalStatusSuccess);

if (reset.reset_type == 0x01) {
    delay(200);           // allow ACK to be forwarded to Jetson over UART
    NVIC_SystemReset();   // hard reset — full MCU reboot
}

// Soft reset: clear sync state, flush TX buffers, re-enter AWAKEN loop
_tdmaClock.reset();
_radio.flushTelemetryBuffers("cmd_reset_soft");
_packetHandler.reset();
_syncActive = false;
sendAwakenHandshake();
_awakenLastSentMs = _clock.millis();
```

After soft reset the node falls into the AWAKEN retry loop in `update()` and resyncs on the next TIME_SYNC from the base.

The `delay(200)` before hard reset gives the base ~200 ms to relay the CMD_ACK over UART to the Jetson before the radio goes down. `sendImmediate()` already blocks until the link-layer ACK from the base arrives (~35 ms round-trip), so the ACK is in the UART buffer before the delay starts.

### Gap 3 — `TdmaClock` has no `reset()` method

**Files**: `platformio/include/radio/TdmaClock.h`, `platformio/src/radio/TdmaClock.cpp`

Gap 2's soft reset calls `_tdmaClock.reset()`, which does not exist. Add:

```cpp
// TdmaClock.h declaration
void reset();

// TdmaClock.cpp definition
void TdmaClock::reset() {
    _hasSync        = false;
    _sessionChanged = false;
    _syncSessionMs  = 0;
    _syncLocalMs    = 0;
}
```

This causes `hasSync()` to return false, which drops the node back into the AWAKEN wait loop in `SmartFiresNodeApp::update()`.

### Gap 4 — Jetson: no base reset on session start

**File**: `edge/edge-receiver/src/smartfires_edge/ingest_service.py`

`run_receive()` does not reset the base on startup or on `reset_event`. Two trigger points need to be wired up.

**Add import** (line ~24):
```python
from smartfires_edge.packet import (
    ...
    encode_cmd_reset_frame,
)
```

**Add helper**:
```python
def _send_base_reset(
    ser: serial.Serial,
    write_lock: threading.Lock,
    cmd_seq_state: dict,
    log_fn: Callable[[str, int | None], None],
) -> bool:
    seq = int(cmd_seq_state.setdefault("next_seq", 0)) & 0xFF
    frame = encode_cmd_reset_frame(node_id=0, reset_type=0, seq=seq)
    with write_lock:
        try:
            ser.write(frame)
        except serial.SerialException as exc:
            print(f"[EDGE][BASE-RESET] write error: {exc}", file=sys.stderr)
            return False
    cmd_seq_state["next_seq"] = (seq + 1) & 0xFF
    log_fn(f"[EDGE][BASE-RESET] seq={seq:03d} reset_type=soft bytes={len(frame)}", None)
    return True
```

**Initialize** a separate `cmd_seq_state = {"next_seq": 0}` dict alongside `sync_state`.

**Trigger 1 — session start** (where sync thread is started, first event in loop):

```python
if not sync_thread_started:
    _send_base_reset(ser, write_lock, cmd_seq_state, log_fn)
    time.sleep(0.5)
    _send_time_sync(
        ser=ser, write_lock=write_lock, sync_state=sync_state,
        session_ctx=session_ctx, reason="session_start", log_fn=log_fn,
    )
    sync_thread = threading.Thread(...)
    sync_thread.start()
    sync_thread_started = True
```

**Trigger 2 — `reset_event`** (around line 394, existing new-session block):

```python
if reset_event is not None and reset_event.is_set():
    reset_event.clear()
    # ...existing session teardown and new session_id creation...

    _send_base_reset(ser, write_lock, cmd_seq_state, log_fn)
    time.sleep(0.5)
    _send_time_sync(..., reason="new_session", ...)
```

The 500 ms sleep gives the RH_RF95 `begin()` call time to complete before the TIME_SYNC is sent. The base typically reinitializes in under 200 ms.

---

## Full Flows

### Session Start

```
Jetson: run_receive() starts
    ↓ first UART byte arrives (serial port open)
    → _send_base_reset(node_id=0, reset_type=soft)
    → sleep(500 ms)
    → _send_time_sync(reason="session_start")
    → base: reinits LoRa radio
    → base: receives TIME_SYNC → updateJetsonTimeSource() → caches new session_id
    → start periodic sync thread
    ↓ node sends AWAKEN (or arrives naturally on next TDMA cycle)
    → Jetson: sends TIME_SYNC on AWAKEN (existing path)
    → node: TdmaClock.applySync(new session_id) → sensing begins
```

### Per-Node Reset (CLI/web command)

```
Operator: "reset node 2" (soft) or "reset node 2 hard"
    → encode_cmd_reset_frame(node_id=2, reset_type=0|1) → UART
    → base: PKT_CMD_RESET, node_id=2 → sendToWait(payload, len, 2) → LoRa
    → node 2: decodeCmdReset, node_id matches
        → sendCmdAck(PKT_CMD_RESET, success) → LoRa
        If hard:
            delay(200) → NVIC_SystemReset() → MCU reboots → AWAKEN loop
        If soft:
            TdmaClock.reset() → hasSync()=false
            flushTelemetryBuffers()
            PacketHandler.reset()
            sendAwakenHandshake()
    → base: forwards CMD_ACK to Jetson over UART
    → Jetson: logs CMD_ACK, waits for AWAKEN
    → Jetson: sends TIME_SYNC on AWAKEN (existing path)
    → node: resync, sensing resumes
```

### Base Reset Only (explicit, e.g. from CLI)

```
Operator: "reset base" (or "reset base hard")
    → encode_cmd_reset_frame(node_id=0, reset_type=0|1) → UART
    → base: PKT_CMD_RESET, node_id=0 → self-reset (not forwarded)
    Soft:
        _hasJetsonTime = false
        _ackTrackers cleared
        _lastPeriodicTimeSyncMs = 0
        _radio.begin()
        → maybeSendPeriodicTimeSync() fires on next update() tick
    Hard:
        NVIC_SystemReset() → MCU reboots
        → base re-initializes, waits for Jetson TIME_SYNC
```

---

## Implementation Phases

| Phase | Scope | Files | Reflash? |
|---|---|---|---|
| 1 | Jetson auto-reset on session start/reset_event | `ingest_service.py`, `packet.py` import | No |
| 2 | Base station self-reset (`node_id == 0`) | `SmartFiresBaseApp.cpp` | Base only |
| 3 | Node reset execution + `TdmaClock::reset()` | `SmartFiresNodeApp.cpp`, `TdmaClock.h/.cpp` | All nodes |
| 4 | CLI/web `reset node <N>` and `reset base` commands | Python web/CLI layer | No |

Phase 1 can be deployed and tested independently (base just ignores the CMD_RESET with `node_id=0` — drops it with a `uart_cmd_unsupported` log line, which is benign). Phase 2 makes it actually take effect on the base. Phase 3 makes per-node resets functional.

---

## Notes

- **No new packet types** — `PKT_CMD_RESET (0x11)` handles both base and node resets via the `node_id` field in `CmdResetPayload`.
- **CMD_ACK round-trip for node resets** — the Jetson already handles `PKT_CMD_ACK` in `ingest_service.py`. A 5-second ACK timeout (already in the CLI plan in `JETSON_CLI_AND_COMMAND_SYSTEM.md`) applies.
- **Base self-reset has no CMD_ACK** — the base cannot ACK itself; the Jetson infers success if the subsequent TIME_SYNC is forwarded correctly.
- **Hard reset timing** — `delay(200)` before `NVIC_SystemReset()` on a node is enough for the base to relay the CMD_ACK over UART (~35 ms LoRa round-trip + ~1 ms UART write). No further delay is needed.
- **`_ackTrackers` vs `_nodeAssignments` on base soft reset** — trackers are cleared because they contain stale per-node sequence state that becomes invalid after radio reinit. Node assignments are preserved so existing nodes don't need to re-AWAKEN with new IDs.

## Known Limitation — `kMaxPendingCommands` vs. node count

**Status: open, must fix before deploying more than 4 nodes.**

The Jetson's "New Session" web flow (`/api/new_session` → `reset_event` handler in
`ingest_service.py`) hard-resets every node in `cfg.nodes` before resetting the base,
by enqueueing one `CMD_RESET` per node in a tight loop (see "Full Flows" — this is an
extension beyond the original per-node-reset design above). Each enqueue lands in the
base's `_pendingCommands` ring (`SmartFiresBaseApp.h`), sized by
`kMaxPendingCommands = 4`. That queue is shared with `CMD_CALIBRATE` and only drains
during the base's own reserved TDMA slot 0 (once per ~3.6s frame at default
`NUM_SLOTS=4`/`slotWidthMs=900`), so it can't be assumed to drain between enqueue calls.

With more than 4 configured nodes, the 5th+ `enqueuePendingCommand()` calls return
`false` (`QUEUE_FULL`) and those nodes silently never get reset — visible only as a
`tx_cmd_reset_queue ... result=QUEUE_FULL` line in the base's debug log
(`/debug` page), not surfaced anywhere in the Jetson UI.

**Fix before scaling past 4 nodes** — either:

- Bump `kMaxPendingCommands` on the base (cheap, bounded by `kPendingCommandPayloadSize`
  per slot; reflash the base), or
- Batch the Jetson-side reset loop: send ≤4 at a time, then wait roughly one TDMA frame
  period before sending the next batch, so the queue has actually drained.

Tracked in code at `SmartFiresBaseApp.h`'s `kMaxPendingCommands` declaration.
