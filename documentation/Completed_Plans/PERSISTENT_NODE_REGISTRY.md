---
name: persistent-node-registry
description: Fix plan for two bugs in uid_hash-to-node_id correlation — a radio-address collision in the AWAKEN handshake and missing Jetson-side mapping persistence.
category: plan-completed
status: historical
superseded_by: tdma-protocol
---

# Node ID Correlation Forwarding

## Goal

Node IDs are assigned entirely by the base station during the AWAKEN handshake — no
Jetson involvement in assignment. The Jetson just needs to know which `uid_hash` maps to
which `node_id` for the current session so that correlation is recorded in the data.

Two bugs prevent this from working correctly today.

---

## Bug 1 — `initialRadioAddr` collision (runtime)

`makeInitialRadioAddr` uses only the lower 6 bits of `uid_hash`, giving 64 possible
temporary radio addresses (0x80–0xBF). If two boards share the same lower 6 bits they
always get the same `initialRadioAddr`. The base then unicasts a TIME_SYNC to that
address and both unassigned nodes receive and accept it, both applying the same
`node_id` and colliding on the same TDMA slot.

### Fix — `platformio/src/main.cpp:92`

```cpp
// Before: 6-bit space (64 values, 0x80–0xBF)
uint8_t makeInitialRadioAddr(uint32_t uidHash) {
    uint8_t addr = static_cast<uint8_t>(0x80u | (uidHash & 0x3Fu));
    if (addr == 0xFFu) { addr = 0x80u; }  // dead code — can never reach 0xFF
    return addr;
}

// After: 8-bit space (254 values)
uint8_t makeInitialRadioAddr(uint32_t uidHash) {
    uint8_t addr = static_cast<uint8_t>(uidHash & 0xFF);
    if (addr == 0x00u) addr = 0x01u;   // 0x00 = RH unassigned
    if (addr == 0xFFu) addr = 0xFEu;   // 0xFF = RH_BROADCAST_ADDRESS
    return addr;
}
```

To verify whether your boards currently collide before flashing:

```bash
SFDBG_SRC=boot SFDBG_MIN_LEVEL=I pio device monitor -e feather_m0_lora_node_debug
```

Both nodes log `radio_addr_init=0xXX` at boot. Identical values confirm the collision.

**Scope:** node firmware only. Reflash both nodes.

---

## Bug 2 — AWAKEN forwarded with `node_id = 0`

The node sends AWAKEN with `PktHeader.node_id = 0` (it is unassigned at that point).
The base assigns a `node_id`, sends the unicast TIME_SYNC, then forwards the raw LoRa
payload to the Jetson via `encodeBaseFrame` — unmodified. So the Jetson always receives
AWAKEN with `node_id = 0` and `on_awaken(0, uid_hash)` maps everything to node 0,
making the session's `uid_hash ↔ node_id` mapping useless.

### Fix — `platformio/src/app/SmartFiresBaseApp.cpp`

In the `PKT_AWAKEN` handler, after `findOrCreateNodeAssignment`, patch a local copy of
the LoRa payload before forwarding:

```cpp
// After: findOrCreateNodeAssignment and sendDirectTimeSync, before encodeBaseFrame
uint8_t patched[BinaryPacket::kAwakenLoRaSize];
memcpy(patched, pkt.data, pkt.len);
if (assignment) {
    patched[offsetof(BinaryPacket::PktHeader, node_id)] = assignment->nodeId;
}
const size_t outLen =
    BinaryPacket::encodeBaseFrame(pkt.rssi, patched, pkt.len, frame, sizeof(frame));
```

The AWAKEN handler currently falls through to the generic `encodeBaseFrame` call at the
bottom of the LoRa RX loop. That call must be skipped for AWAKEN (it is now handled
above), or the frame will be forwarded twice — once patched, once with `node_id = 0`.
Use a flag or `continue` to skip the generic forward for `PKT_AWAKEN`.

**Scope:** base station firmware only. Reflash base.

---

## Jetson side — no change needed

`session.py:on_awaken` already records `node_id → uid_hash` and `uid_hash → node_id`
into `session.json` and saves it to disk. Once Bug 2 is fixed, `hdr_node` in
`ingest_service.py:188` will be the real assigned `node_id` and the mapping will be
correct for the session.

`session.json` persists across `smartfires-edge receive` restarts but is intentionally
session-scoped — if the base reboots and reassigns IDs in a different order, the mapping
is overwritten on the next AWAKEN. This is acceptable: the base is the authority for
node assignment, and the Jetson simply records whatever the base decided.

---

## Files changed

| File | Change |
|---|---|
| `platformio/src/main.cpp` | Fix `makeInitialRadioAddr` — 6-bit → 8-bit address space |
| `platformio/src/app/SmartFiresBaseApp.cpp` | Patch `node_id` in AWAKEN UART forward; skip generic forward for `PKT_AWAKEN` |

---

## Out of scope

- Persistent cross-session node ID stability (base reboots can change assignment order;
  accepted as a known limitation for now).
- CMD_REASSIGN or any Jetson-driven correction of assignments.
