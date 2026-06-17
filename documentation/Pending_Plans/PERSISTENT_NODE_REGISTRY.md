# Persistent Node ID Registry

## Problem

The base station's `uid_hash → node_id` table is RAM-only (`_nodeAssignments`). After
a base reboot, IDs are assigned in first-AWAKEN-first-served order. If nodes come up in
a different order than the previous session, they receive different IDs, which corrupts
per-node data correlation in the Jetson's CSV and `session.json`.

### Existing state worth noting

`session.json` already has `uid_hash_to_node_id` and is already persisted to disk.
However it is currently populated from **STATUS packets** (not AWAKEN) because the AWAKEN
LoRa payload always carries `PktHeader.node_id = 0` (the node is unassigned at AWAKEN
time), so `on_awaken(0, uid_hash)` maps everything to node 0 and is effectively a no-op.

## Approach

Three changes, layered bottom-up:

1. **Base firmware** — store the node's temporary radio address, and patch the forwarded
   AWAKEN UART frame so the Jetson sees the actual assigned `node_id`.
2. **New command `CMD_REASSIGN`** — Jetson can instruct the base to move a node to a
   different `node_id` and push that correction to the node via a unicast TIME_SYNC.
3. **Jetson** — maintain a permanent `node_registry.json`, detect mismatches on AWAKEN,
   send `CMD_REASSIGN` when needed.

---

## Change 1 — Base firmware: store `radioAddr` + patch AWAKEN forward

### 1a. Store temporary radio address in `NodeAssignment`

`SmartFiresBaseApp.h` — add `radioAddr` to `NodeAssignment`:

```cpp
struct NodeAssignment {
    bool    inUse    = false;
    uint8_t radioAddr = 0;   // ← NEW: pkt.from at AWAKEN time
    uint8_t nodeId   = 0;
    uint32_t uidHash = 0;
};
```

`SmartFiresBaseApp.cpp` — `findOrCreateNodeAssignment` takes a second arg:

```cpp
NodeAssignment* findOrCreateNodeAssignment(uint32_t uidHash, uint8_t radioAddr);
// Sets freeAssignment->radioAddr = radioAddr on new entries.
// Does NOT overwrite radioAddr on returning entries (node might have rebooted,
// but its radio addr is stable until base reboots).
```

Call site (`PKT_AWAKEN` handler) passes `pkt.from`:

```cpp
NodeAssignment* assignment =
    findOrCreateNodeAssignment(awaken.uid_hash, pkt.from);
```

### 1b. Patch forwarded AWAKEN frame with assigned `node_id`

The base currently calls `encodeBaseFrame(pkt.rssi, pkt.data, pkt.len, ...)` with the
raw LoRa payload (node_id=0). Before calling `encodeBaseFrame`, patch a local copy:

```cpp
// In the PKT_AWAKEN handling block, after findOrCreateNodeAssignment:
uint8_t patched[BinaryPacket::kAwakenLoRaSize];
memcpy(patched, pkt.data, pkt.len);
if (assignment) {
    patched[offsetof(BinaryPacket::PktHeader, node_id)] = assignment->nodeId;
}
BinaryPacket::encodeBaseFrame(pkt.rssi, patched, pkt.len, frame, sizeof(frame));
// Do NOT call encodeBaseFrame again on pkt.data below — skip the normal forward path
// for AWAKEN (it is handled here).
```

`kAwakenLoRaSize` is already defined in `BinaryPacket.h` (= 9).

After this change the Jetson's `on_awaken` call receives the real assigned `node_id`
instead of 0.

---

## Change 2 — New `CMD_REASSIGN` command (Jetson → Base → Node)

### 2a. `BinaryPacket.h` — add packet type and payload

```cpp
PKT_CMD_REASSIGN = 0x14,   // Jetson → Base: move a node to a canonical node_id

struct __attribute__((packed)) CmdReassignPayload {
    uint32_t uid_hash;      // which node to reassign
    uint8_t  new_node_id;   // the canonical node_id to assign
};
// Total UART payload: PktHeader(4) + CmdReassignPayload(5) + crc8(1) = 10 bytes
```

Add encoder (`encodeReassignPayload`) and decoder (`decodeReassign`) following the
pattern of existing CMD encoders/decoders.

### 2b. `SmartFiresBaseApp.cpp` — handle CMD_REASSIGN from Jetson UART

In the Jetson UART frame dispatch (where `PKT_CMD_CALIBRATE`/`PKT_CMD_RESET` are
handled), add:

```cpp
if (hdr.pkt_type == BinaryPacket::PKT_CMD_REASSIGN) {
    BinaryPacket::CmdReassignPayload cmd = {};
    if (!BinaryPacket::decodeReassign(payload, len, hdr, cmd)) { return; }

    NodeAssignment* a = findAssignmentByUidHash(cmd.uid_hash);
    if (!a) {
        LOG_WARN("base", "reassign_no_entry uid=0x%08lX", cmd.uid_hash);
        return;
    }

    a->nodeId = cmd.new_node_id;
    sendDirectTimeSync(a->radioAddr, cmd.new_node_id, "reassign", hdr.seq);

    LOG_INFO("base", "reassign_applied uid=0x%08lX new_node=%u radio=%u",
             cmd.uid_hash, cmd.new_node_id, a->radioAddr);
}
```

Add `findAssignmentByUidHash(uint32_t) → NodeAssignment*` (searches `_nodeAssignments`
by `uidHash`; same loop pattern as `findOrCreateNodeAssignment`).

The unicast TIME_SYNC is received by the node → `applyAssignedNodeId` → correct slot.

### 2c. `packet.py` — encoder for CMD_REASSIGN

```python
PKT_CMD_REASSIGN = 0x14
CMD_REASSIGN_PAYLOAD_FMT  = "<IB"   # uid_hash(u32) + new_node_id(u8)
CMD_REASSIGN_PAYLOAD_SIZE = struct.calcsize(CMD_REASSIGN_PAYLOAD_FMT)  # 5

def encode_cmd_reassign(uid_hash: int, new_node_id: int) -> bytes:
    """Returns a complete UART frame ready to write to serial."""
    lora_payload = _encode_header(PKT_CMD_REASSIGN, new_node_id, seq=0)
    lora_payload += struct.pack(CMD_REASSIGN_PAYLOAD_FMT, uid_hash, new_node_id)
    lora_payload += bytes([crc8(lora_payload)])
    return encode_uart_frame(lora_payload)
```

---

## Change 3 — Jetson: `node_registry.json` + correction logic

### 3a. New file: `node_registry.py`

A lightweight persistent store separate from `session.json`.
`session.json` is session-scoped metadata (heading, last_seen, etc.).
`node_registry.json` is permanent identity data — survives session resets.

Default location: `~/.smartfires/node_registry.json`

```python
# node_registry.json on disk:
{
  "0xA1B2C3D4": 2,
  "0xDEADBEEF": 3
}
```

Public interface:

```python
class NodeRegistry:
    def __init__(self, path: Path | None = None): ...

    def lookup_or_register(
        self, uid_hash: int, base_assigned_id: int
    ) -> tuple[int, bool]:
        """
        Returns (canonical_node_id, needs_correction).
        - If uid_hash is new: register it with base_assigned_id, return (base_assigned_id, False).
        - If uid_hash known and matches: return (canonical_id, False).
        - If uid_hash known but mismatch: return (canonical_id, True).
        """
```

Uses the same `atomic_write_json` / `read_json` helpers as `session.py`.
Thread-safe (`threading.Lock`).

### 3b. `ingest_service.py` — instantiate registry, check on AWAKEN

```python
registry = NodeRegistry(path=data_dir / "node_registry.json")
```

In the AWAKEN handler (currently `session_manager.on_awaken(int(hdr_node), int(uid_hash))`):

```python
base_assigned_id = int(hdr_node)   # now non-zero after Change 1b
canonical_id, needs_correction = registry.lookup_or_register(uid_hash, base_assigned_id)

if needs_correction:
    frame = encode_cmd_reassign(uid_hash, canonical_id)
    with write_lock:
        ser.write(frame)
    print(f"[EDGE][REASSIGN] uid=0x{uid_hash:08x} base={base_assigned_id} → canonical={canonical_id}")

# Still record in session manager with the canonical id
session_manager.on_awaken(canonical_id, uid_hash)
```

---

## Files changed

| File | Change |
|---|---|
| `platformio/include/telemetry/BinaryPacket.h` | Add `PKT_CMD_REASSIGN`, `CmdReassignPayload`, encoder + decoder |
| `platformio/include/app/SmartFiresBaseApp.h` | Add `radioAddr` to `NodeAssignment`; declare `findAssignmentByUidHash` |
| `platformio/src/app/SmartFiresBaseApp.cpp` | `findOrCreateNodeAssignment` takes `radioAddr`; patch AWAKEN forward; handle `CMD_REASSIGN` |
| `edge/.../packet.py` | Add `PKT_CMD_REASSIGN`, `encode_cmd_reassign`, UART frame helper |
| `edge/.../node_registry.py` | New module — `NodeRegistry` class |
| `edge/.../ingest_service.py` | Instantiate `NodeRegistry`; check + correct on AWAKEN |

---

## Sequence after base reboot (correct-order example)

```
Node A (uid=0xAAAA, canonical=2) wakes first:
  Node  → Base:   AWAKEN uid=0xAAAA
  Base assigns 2 (first free slot = kFirstNodeId + 0)
  Base  → Jetson: AWAKEN uid=0xAAAA node_id=2  ← patched
  Jetson: registry.lookup(0xAAAA, 2) → (2, False)  — no correction needed
  Base  → Node A: TIME_SYNC node_id=2

Node B (uid=0xBBBB, canonical=3) wakes second:
  Node  → Base:   AWAKEN uid=0xBBBB
  Base assigns 3 (next free slot)
  Base  → Jetson: AWAKEN uid=0xBBBB node_id=3  ← patched
  Jetson: registry.lookup(0xBBBB, 3) → (3, False)  — no correction needed
  Base  → Node B: TIME_SYNC node_id=3
```

## Sequence after base reboot (wrong-order example)

```
Node B (uid=0xBBBB, canonical=3) wakes first:
  Node  → Base:   AWAKEN uid=0xBBBB
  Base assigns 2 (first free slot)
  Base  → Jetson: AWAKEN uid=0xBBBB node_id=2  ← patched
  Jetson: registry.lookup(0xBBBB, 2) → (3, True)  — mismatch!
  Jetson → Base:  CMD_REASSIGN uid=0xBBBB new_node_id=3
  Base: updates _nodeAssignments[B].nodeId = 3
  Base  → Node B: TIME_SYNC node_id=3  (unicast to B's radioAddr)
  Node B: applyAssignedNodeId(3) → slot 2, correct

Node A (uid=0xAAAA, canonical=2) wakes second:
  Node  → Base:   AWAKEN uid=0xAAAA
  Base assigns 2 (slot 0 is now free again — B moved to 3)
  Base  → Jetson: AWAKEN uid=0xAAAA node_id=2  ← patched
  Jetson: registry.lookup(0xAAAA, 2) → (2, False)  — correct
  Base  → Node A: TIME_SYNC node_id=2
```

## Out of scope / future

- Handling the case where `kMaxAssignedNodes` is exhausted (base returns `nullptr` from
  `findOrCreateNodeAssignment`). Currently this silently drops the node. A log warning
  already covers it; no change needed here.
- Persisting `NodeAssignment` to base-station flash so the base itself survives reboots
  without needing Jetson correction. Not needed while the Jetson is always online.
- `NUM_SLOTS` coordination: adding a node still requires reflashing all node Feathers
  with the new `NUM_SLOTS`. This plan doesn't change that.
