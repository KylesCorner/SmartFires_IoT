# TDMA & Bundle Packet Sizing Reference

This document derives the relationships between sensing rate, bundle delta count, LoRa
packet size, LoRa airtime, TDMA slot width, and the number of nodes. Use it to choose
parameters when adding nodes, changing sensing rate, or tuning reliability.

---

## 1  Variable Definitions

| Symbol | Name | Current value |
|--------|------|---------------|
| f_s | Sensing rate (Hz) | **4 Hz** |
| N_δ | Delta samples per bundle | **7** |
| N_n | Number of TDMA nodes | 2 |
| W | Slot width (ms) | **900 ms** |
| G | Guard time per slot edge (ms) | 20 ms |
| R | LoRa retries (re-attempts after first TX) | **1** |
| T_ack | ACK timeout per attempt (ms) | 100 ms |
| SF | LoRa spreading factor | 7 |
| BW | LoRa bandwidth (kHz) | 125 |
| CR | Coding rate denominator (4/CR notation) | 5 (i.e. 4/5) |

RadioHead adds a 4-byte framing header (TO, FROM, ID, FLAGS) to every LoRa packet.

---

## 2  Bundle Packet Size

The LoRa payload for a `PKT_BUNDLE` frame:

```
L  =  4  (PktHeader)
    + 32  (FullStatePayload — reference sample)
    +  1  (delta_count byte)
    + N_δ × 20  (DeltaPayload per delta sample)

L  =  37  +  20 × N_δ          [bytes]
```

On-air byte count (including RadioHead 4-byte header):

```
L_air  =  L + 4  =  41  +  20 × N_δ     [bytes]
```

Comparison to the legacy `PKT_FULL_STATE` (no deltas):

```
L_full_state  =  4 + 32  =  36 bytes  →  L_air = 40 bytes
```

---

## 3  LoRa Airtime

Using the standard SX127x time-on-air formula for SF=7, BW=125 kHz, CR=4/5,
8-symbol preamble, explicit header mode:

```
T_sym   =  2^SF / (BW × 10³)  =  128 / 125 000  =  1.024 ms

T_pre   =  (8 + 4.25) × T_sym  =  12.25 × 1.024  ≈  12.54 ms

n_sym   =  8  +  ⌈ (8 × L_air + 16) / (4 × SF) ⌉  ×  (CR_num + 4)
        =  8  +  ⌈ (8 × L_air + 16) / 28 ⌉  ×  5        (SF=7, CR=4/5)

T_tx    =  (12.25 + n_sym) × 1.024                        [ms]
```

The `n_sym` formula counts the 8 explicit-header symbols separately from the payload
symbol blocks. Each block is 28 chips wide at SF=7, and CR=4/5 adds 1 redundant symbol
per 4 data symbols (×5 factor).

### Calibration check

At N_δ = 0, L = 36, L_air = 40:
```
n_sym  =  8 + ⌈(320 + 16)/28⌉ × 5  =  8 + 12×5  =  68
T_tx   =  (12.25 + 68) × 1.024  =  82.2 ms  ✓  (observed ≈ 82 ms)
```

---

## 4  Computed T_tx by Delta Count

| N_δ | L (bytes) | L_air (bytes) | n_sym | T_tx (ms) |
|-----|-----------|----------------|-------|-----------|
| 0   | 36        | 40             | 68    | 82        |
| 1   | 57        | 61             | 98    | 113       |
| 2   | 77        | 81             | 128   | 144       |
| 3   | 97        | 101            | 158   | 174       |
| 4   | 117       | 121            | 188   | 205       |
| 5   | 137       | 141            | 213   | 231       |
| 6   | 157       | 161            | 243   | 261       |
| 7   | 177       | 181            | 273   | 292       |
| 8   | 197       | 201            | 298   | 318       |
| 9   | 217       | 221            | 328   | 348       |
| 10  | 237       | 241            | 358   | 379       |

---

## 5  TDMA Slot Width Constraint

Each node must complete its entire transmission (including retries) before the guard
band of the next node's slot:

```
TX window  =  W - 2G                                    [ms]

Worst-case TX time  =  (R + 1) × (T_tx + T_ack)        [ms]

Constraint:  (R + 1) × (T_tx + T_ack)  ≤  W - 2G

∴  W  ≥  (R + 1) × (T_tx + T_ack)  +  2G              [ms]
```

### Required slot width W_min by retry count

| N_δ | T_tx (ms) | W_min at R=0 (ms) | W_min at R=1 (ms) | W_min at R=2 (ms) | W_min at R=3 (ms) |
|-----|-----------|-------------------|-------------------|-------------------|-------------------|
| 0   | 82        | 222               | 404               | 584               | 768               |
| 1   | 113       | 253               | 466               | 679               | 892               |
| 2   | 144       | 284               | 528               | 774               | 1016              |
| 3   | 174       | 314               | 588               | 864               | 1136              |
| 4   | 205       | 345               | 650               | 960               | 1260              |
| 5   | 231       | 371               | 702               | 1040              | 1364              |
| 6   | 261       | 401               | 764               | 1130              | 1484              |
| **7** | **292** | **432**           | **824 → 900**     | 1224              | 1608              |
| 8   | 318       | 458               | 882               | 1306              | 1712              |
| 9   | 348       | 488               | 944               | 1396              | 1832              |
| 10  | 379       | 519               | 1008              | 1490              | 1956              |

*Current config: N_δ=7, R=1, W=900 ms (76 ms above W_min=824 ms).*

---

## 6  Bundle Accumulation Time

The ESP32 accumulates one sample every T_sense = 1/f_s seconds. A full bundle holds
1 reference + N_δ deltas = N_δ + 1 samples:

```
T_bundle  =  (N_δ + 1) / f_s                           [seconds]
```

| N_δ | T_bundle at 1 Hz (s) | T_bundle at 2 Hz (s) | T_bundle at 4 Hz (s) |
|-----|----------------------|----------------------|----------------------|
| 0   | 1.0                  | 0.5                  | 0.25                 |
| 1   | 2.0                  | 1.0                  | 0.5                  |
| 2   | 3.0                  | 1.5                  | 0.75                 |
| 3   | 4.0                  | 2.0                  | 1.0                  |
| 4   | 5.0                  | 2.5                  | 1.25                 |
| 5   | 6.0                  | 3.0                  | 1.5                  |
| 6   | 7.0                  | 3.5                  | 1.75                 |
| **7** | **8.0**            | **4.0**              | **2.0**              |
| 8   | 9.0                  | 4.5                  | 2.25                 |
| 9   | 10.0                 | 5.0                  | 2.5                  |
| 10  | 11.0                 | 5.5                  | 2.75                 |

At N_δ=7, f_s=4 Hz: T_bundle = 2.0 s.

---

## 7  Queue Pressure and Data Loss

### How the queue works

The Feather holds a 4-slot ring buffer. The ESP32 pushes bundles in; the Feather
pops and transmits one per TDMA slot. When the queue is full, the oldest entry is
evicted to make room — the system always holds the freshest data.

### The key ratio: η

```
η  =  T_frame / T_bundle
   =  (N_n × W) / (T_bundle × 1000)
```

- **η < 1** — radio drains the queue faster than the ESP32 fills it. No data loss.
- **η = 1** — perfect balance. Queue depth stays around 1.
- **η > 1** — ESP32 produces bundles faster than the radio can send them.
  Once the 4-slot queue saturates (~34 seconds after startup), every new bundle
  evicts the oldest one still waiting.

**Steady-state data loss rate when η > 1:**

```
loss  =  (η − 1) / η
```

For example: η = 1.3 → loss = 0.3/1.3 = **23%** of bundles are dropped before TX.
Each dropped bundle at 4 Hz represents 8 sensor samples = 2 seconds of data.

### Current config at 4 Hz (N_δ=7, R=1, W=900 ms)

```
T_bundle  =  2.0 s
T_frame   =  2 × 900 ms  =  1.8 s
η         =  1800 / 2000  =  0.90   →  no data loss ✓
```

The radio drains bundles 11% faster than the ESP32 produces them.

### What happens as nodes are added

The table below uses the **current** N_δ=7 and W=900 ms (fixed for all nodes).

| N_n | T_frame (ms) | η    | Loss rate | Impact |
|-----|--------------|------|-----------|--------|
| 2   | 1800         | 0.90 | 0%        | ✓ healthy |
| 3   | 2700         | 1.35 | 26%       | ✗ significant loss |
| 4   | 3600         | 1.80 | 44%       | ✗ nearly half the data dropped |
| 5   | 4500         | 2.25 | 56%       | ✗ not viable |

Adding a third node with no other changes drops more than a quarter of all data.

### Fixes when adding nodes

The root cause is that each extra node adds one full slot-width to T_frame, but
T_bundle stays fixed. There are three levers:

**1. Increase N_δ** (larger bundles take longer to accumulate, raising T_bundle).

| Config | N_n | N_δ | R | W (ms) | T_bundle (s) | T_frame (ms) | η    | Loss |
|--------|-----|-----|---|--------|--------------|--------------|------|------|
| Current | 2  | 7   | 1 | 900    | 2.0          | 1800         | 0.90 | 0%   |
| 3 nodes | 3  | 7   | 1 | 900    | 2.0          | 2700         | 1.35 | 26%  |
| 3 nodes | 3  | 10  | 1 | 1050   | 2.75         | 3150         | 1.15 | 13%  |
| 4 nodes | 4  | 10  | 1 | 1050   | 2.75         | 4200         | 1.53 | 35%  |

N_δ=10 is the RadioHead ceiling (L=237 B); 3 nodes still incurs ~13% loss.

**2. Reduce sensing rate** (doubles T_bundle at 2 Hz, halves the problem).

| Config | N_n | f_s | N_δ | W (ms) | T_bundle (s) | T_frame (ms) | η    | Loss |
|--------|-----|-----|-----|--------|--------------|--------------|------|------|
| 3 nodes | 3  | 2 Hz | 7  | 900    | 4.0          | 2700         | 0.68 | 0%   |
| 4 nodes | 4  | 2 Hz | 7  | 900    | 4.0          | 3600         | 0.90 | 0%   |
| 5 nodes | 5  | 2 Hz | 7  | 900    | 4.0          | 4500         | 1.13 | 11%  |

2 Hz sensing cleanly supports 4 nodes with no data loss.

**3. Drop retries to R=0** (halves W_min, shrinks T_frame, accepts no ACK safety net).

| Config | N_n | f_s | R | W (ms) | T_bundle (s) | T_frame (ms) | η    | Loss |
|--------|-----|-----|---|--------|--------------|--------------|------|------|
| 4 nodes | 4  | 4 Hz | 0 | 500   | 2.0          | 2000         | 1.00 | 0%   |
| 5 nodes | 5  | 4 Hz | 0 | 500   | 2.0          | 2500         | 1.25 | 20%  |

R=0 means one TX attempt. If the base misses it, the bundle is gone. Viable only with
strong and consistent RSSI (e.g., short-range fixed nodes with good LOS).

**Summary recommendation by node count at 4 Hz:**

| Nodes | Recommended config | Notes |
|-------|--------------------|-------|
| 2     | N_δ=7, R=1, W=900  | Current config, no data loss |
| 3     | N_δ=7, R=1, W=900, f_s=2 Hz | Drop to 2 Hz, or accept 26% loss at 4 Hz |
| 4     | N_δ=7, R=1, W=900, f_s=2 Hz | Works cleanly at 2 Hz |
| 5+    | Revisit MAC scheme (see §9) | TDMA at 4 Hz approaches RadioHead limits |

---

## 8  RadioHead Max Payload Constraint

RadioHead RH_RF95 caps the LoRa payload at **251 bytes** (255 max frame − 4 RH header).

```
L  ≤  251  →  37 + 20 × N_δ  ≤  251  →  N_δ  ≤  10.7  →  N_δ_max  =  10
```

At N_δ=10: L=237 bytes, L_air=241 bytes, T_tx=379 ms.

This is a hard ceiling. Any approach requiring N_δ > 10 needs a different radio library
or raw SX127x register access (which would allow up to 255 bytes on-air).

---

## 9  Alternative Channel Access Schemes

TDMA is a good fit for this network — deterministic, no coordination overhead at runtime,
no hidden-node problem — but it struggles when T_frame grows faster than T_bundle.
These alternatives are worth considering as the node count rises.

### CSMA/CA with LoRa CAD

**How it works:** Before transmitting, a node runs LoRa's built-in Channel Activity
Detection (CAD) mode (~4 symbol durations ≈ 4 ms at SF7). If the channel is clear, it
transmits immediately. If busy, it waits a random backoff and tries again.

**Pros:** No time synchronisation needed. Adapts naturally to varying traffic. Handles
bursty nodes well.

**Cons:** Hidden-node problem — two nodes that can't hear each other (due to terrain)
both think the channel is free and collide at the base. Latency is non-deterministic.
Throughput drops sharply above ~30% channel utilisation.

**When to use:** Up to ~4–5 nodes with sporadic or low-duty-cycle traffic. Easy to
implement on top of the existing RadioHead library (CAD is one API call on RH_RF95).

---

### SF Orthogonality (Quasi-CDMA)

**How it works:** LoRa signals at different spreading factors are nearly orthogonal —
a base receiver on SF7 largely ignores an SF8 transmission on the same frequency.
Each node uses a distinct SF (SF7, SF8, SF9, …).

**Pros:** No synchronisation. No slot coordination. Works transparently underneath the
existing packet format.

**Cons:** Higher SF = longer airtime (SF8 is 2× slower than SF7, SF9 is 4×). Maximum
~4 orthogonal SFs before SX127x limits. Orthogonality is imperfect at short range.

**When to use:** 2–4 nodes where airtime budget can afford higher SF. A good complement
to TDMA for a heterogeneous fleet (fast near-nodes on SF7, slow far-nodes on SF9).

---

### GPS-Disciplined TDMA

**How it works:** Each Feather node uses a GPS PPS (pulse-per-second) signal for
sub-millisecond time synchronisation instead of the current software TIME_SYNC over LoRa.
Guard time shrinks from 20 ms to 2–3 ms, recovering ~35 ms of TX window per slot.

**Pros:** 10× tighter guard bands. Slots can shrink accordingly (W_min drops ~35 ms per
slot). Supports more nodes in the same frame period. The PA1010D GPS already on each
drone provides the PPS pin.

**Cons:** Requires wiring the PA1010D PPS pin to the Feather, and firmware changes to
latch the GPS epoch. The base station also needs a PPS reference.

**When to use:** When node count exceeds 4 at 4 Hz. Reduces W_min by ~35 ms per slot,
recovering ~8% more frame time — not dramatic on its own, but combines well with larger N_δ.

---

### Dynamic / Polled TDMA

**How it works:** The base station polls nodes in sequence (sends a short "your turn"
packet). A node only transmits after receiving its poll. No node needs a local clock.

**Pros:** No drift, no guard time needed, no TIME_SYNC overhead. Naturally handles
unequal data rates (busy nodes get polled more often).

**Cons:** Adds a downlink packet per TX cycle, doubling channel usage. Latency is
bounded by polling period. A base station failure silences all nodes.

**When to use:** Star topologies where the base is always reliable and low latency is
not critical. Good option if LoRa downlink reliability is proven.

---

### FDMA (Multiple Frequencies)

**How it works:** Each node transmits on a different LoRa channel (e.g., 915.0, 915.2,
915.4 MHz). Nodes never collide because they never share a frequency.

**Pros:** Zero inter-node interference regardless of node count or sensing rate.
Deterministic, no timing coordination.

**Cons:** The SX127x can only listen on one frequency at a time. The base station needs
one radio per node (or a wideband SDR receiver). Hardware cost scales linearly with nodes.

**When to use:** Fixed installations where hardware cost is acceptable. A Raspberry Pi
with multiple SX127x modules or a single SDR (e.g., RTL-SDR + LoRa decoder) could
serve as the base for 4–8 nodes.

---

### Scheme Comparison

| Scheme | Time sync needed | Collision-free | Node count limit | Complexity |
|--------|-----------------|----------------|------------------|------------|
| TDMA (current) | Yes (TIME_SYNC) | Yes | ~4 at 4 Hz | Low |
| CSMA/CA + CAD | No | No (best-effort) | ~4–5 | Low |
| SF orthogonality | No | Mostly | ~4 (SF7–SF10) | Very low |
| GPS TDMA | Yes (PPS) | Yes | ~8+ at 4 Hz | Medium |
| Polled TDMA | No | Yes | Limited by poll rate | Medium |
| FDMA | No | Yes | Hardware-limited | High |

---

## 10  Design Space Summary

**Current configuration** (compiled into firmware):

| Parameter | Value | Constraint satisfied? |
|-----------|-------|-----------------------|
| f_s | 4 Hz | — |
| N_δ | 7 | N_δ_max = 10 ✓ |
| R | 1 | — |
| W | 900 ms | W_min = 824 ms ✓ (76 ms margin) |
| TX window | 860 ms | 2×(292+100) = 784 ms ✓ |
| T_bundle | 2.0 s | — |
| T_frame (2 nodes) | 1.8 s | — |
| η (2 nodes) | 0.90 | < 1 ✓ no data loss |

**Choosing W:** Round up from W_min by at least 50 ms. The Feather M0 crystal drift is
≤1.5 ms per 30 s sync interval at 50 ppm; the 20 ms guard provides ×13 headroom.

**Adding a node:** Check η = (N_n × W × f_s) / (1000 × (N_δ + 1)). If η > 1,
either increase N_δ, reduce f_s, or widen W (and re-verify slot constraint).
