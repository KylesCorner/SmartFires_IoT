---
name: watchdog-timer
description: Plan to add a hardware watchdog to node firmware (Phase 1) and the base station firmware (Phase 2) so an unrecoverable hang self-heals via reboot instead of requiring a manual power cycle.
category: plan-pending
status: draft
last_verified: 2026-07-13
related_docs:
  - tdma-protocol
  - reset-system
  - tunable-parameters
  - packet-reliability
---

# Watchdog Timer

## Reading this doc cold

This plan assumes no other context. If you're picking this up fresh: the short version is
that this firmware has several places where a single missed radio interrupt can hang the
entire single-threaded main loop forever, with no further log output and no recovery short
of a human physically power-cycling the board. Two of the four known hang mechanisms have
already been fixed at the call-site level (see "Current state" below); the remaining ones
can't be fixed that way, for reasons explained in detail in this doc. A hardware watchdog
timer (WDT) is the general-purpose backstop for all of them — it doesn't need to know which
subsystem wedged, and it converts "dead until someone drives out and power-cycles it" into
"dead for at most one WDT period." This plan has not been implemented yet
(`status: draft`) — no `Adafruit_SleepyDog` reference exists anywhere in `platformio.ini`
as of this writing.

## Background

Field reports: nodes occasionally stop responding entirely (no telemetry, no AWAKEN
retries, no debug log activity) and only recover after a manual reset/power cycle.
Diagnosis (across several sessions, working from real device logs, not just code
inspection) turned up **four** concrete hang/stall mechanisms. Two are now fixed at the
call site; two are not, and can't be without either forking a vendored dependency or
building something meaningfully riskier than a hardware watchdog. This section documents
all four, in enough detail that no prior conversation context is needed to understand why
the remaining ones are scoped the way they are.

### Current state (read this first)

| # | Mechanism | Status | Where |
|---|---|---|---|
| 1 | RadioHead auto-ACK-on-receive (`RHReliableDatagram::acknowledge()`, vendored, protected) | **Fixed** | Node's `TdmaRadioService::checkIncomingTimeSync()` now receives with `autoAck=false` and acks explicitly via `RadioHeadTdmaDriver::acknowledge()`, a hand-rolled non-blocking-but-bounded replacement |
| 2 | `RH_RF95::send()`'s own leading `waitPacketSent()` (fires on *every* send, not just replies) | **Fixed** | `RadioHeadTdmaDriver::send()` now waits (bounded) for its own transmission before returning |
| 3 | `RHReliableDatagram::sendtoWait()`'s internal `waitPacketSent()`, inside its own retry loop | **Open — no call-site fix possible** | 6 call sites across node and base, enumerated below |
| 4 | Shared I2C bus (`Wire.endTransmission()`/`requestFrom()`), no timeout, no bus-recovery sequence | **Open — untouched** | Any SHT31/ICM-20948/GPS driver call |

Mechanisms 3 and 4 are what this watchdog plan exists to catch. Mechanisms 1 and 2 are
included here for full context and because the watchdog remains valuable defense-in-depth
even for fixed call sites — a bounded wait is not a *proof* nothing can go wrong, just a
strong mitigation.

### Mechanism 1 — auto-ACK-on-receive (fixed)

RadioHead's `RHGenericDriver::waitPacketSent()` (no-arg overload) is:

```cpp
bool RHGenericDriver::waitPacketSent()
{
    while (_mode == RHModeTx)
        YIELD;          // no timeout — depends on the TX-done IRQ firing
    return true;
}
```

`_mode` only leaves `RHModeTx` from the DIO0 interrupt handler
(`RH_RF95::handleInterrupt()`). DIO0 is edge-triggered
(`attachInterrupt(..., RISING)`, `RH_RF95.cpp:108`) rather than level-triggered, which the
vendored library's own source comments (`RH_RF95.cpp:153-174`) acknowledge as a known
"slim chance of missing events." A missed edge is gone forever — nothing re-triggers it,
and the call spins forever.

This was confirmed in the field: a device log caught the node's auto-ACK-on-receive of an
`ACK_SUMMARY` packet (routed automatically through RadioHead's protected
`RHReliableDatagram::acknowledge()`, fired whenever `recvfromAck()` accepts a unicast
datagram) hang for ~115 s before recovering on its own — proving it was a genuine
wait-condition stall (not memory corruption: the same call path had already succeeded
hundreds of times earlier in that same session).

**Fix:** the node now calls `_driver.receive(packet, /*autoAck=*/false)`, disabling
RadioHead's automatic ACK-on-receive entirely (matching what `SmartFiresBaseApp` already
did for its own receive path, for `PKT_AWAKEN`). For the packet types that still need an
ACK for the sender's `sendToWait()` to succeed (`ACK_SUMMARY`, `CMD_CALIBRATE`/
`CMD_RESET`, direct/unicast `TIME_SYNC` — never the periodic broadcast, see below), the
node calls `_driver.acknowledge(packet.from, packet.id)` explicitly.
`RadioHeadTdmaDriver::acknowledge()` reconstructs the same ACK frame RadioHead's own
`acknowledge()` would send (`setHeaderId`/`setHeaderFlags`/`sendto` — all public RadioHead
primitives; only `acknowledge()` itself is protected), then waits for it to physically
finish transmitting via the **bounded** `RHGenericDriver::waitPacketSent(timeout)`
overload, capped at `NetworkConfig::kAckTxWaitMs` (currently reuses
`kAwakenTxBudgetMs` = 90 ms, since the ACK payload is smaller than AWAKEN's). A timeout
logs `ack_tx_timeout` rather than being treated as failure — the SX1276 may well have
finished transmitting despite a missed interrupt; the timeout only means we couldn't
confirm it.

`ITdmaRadioDriver::ReceivedPacket` gained a `to` field (previously discarded) so the
receiver can tell a broadcast receipt apart from a direct one — acking a broadcast would
make every node on the channel reply at once and collide with each other.

**A history note worth keeping, because it's instructive:** the *first* version of this
fix had `acknowledge()` return immediately after `sendto()`, with **no wait of any
kind** — reasoning that since an unbounded wait caused the original hang, removing the
wait entirely must be safe. It wasn't. Nothing then guaranteed the ACK had physically
finished transmitting before `TdmaRadioService::updateRxPower()`'s `sleep()` call (or, in
principle, any other radio operation) could act next — and `RH_RF95::sleep()` has **no
guard against an in-flight transmission**:

```cpp
bool RH_RF95::sleep() {
    if (_mode != RHModeSleep) {
        modeWillChange(RHModeSleep);
        spiWrite(RH_RF95_REG_01_OP_MODE, RH_RF95_MODE_SLEEP);
        _mode = RHModeSleep;
    }
    return true;
}
```

It unconditionally commands sleep mode regardless of what the radio is doing. This
surfaced in the field as the base station retransmitting `ACK_SUMMARY` far more than
expected, because nodes weren't reliably completing their ACK before the radio got put
back to sleep mid-transmission. **The lesson: "not waiting at all" and "waiting
unboundedly" are not the only two options, and the first one has its own failure mode.**
The bounded wait is what actually closes the gap — the transmission is guaranteed to
either finish or definitively time out before the function returns, so nothing downstream
can act on the radio mid-transmission.

### Mechanism 2 — `RH_RF95::send()`'s own leading wait (fixed)

Easy to miss: RadioHead's "fire-and-forget" `send()` is not actually free of this risk,
because `RH_RF95::send()` itself opens with the same unbounded `waitPacketSent()` — *not*
waiting for its own transmission, but waiting (unconditionally, unbounded) for whatever
transmission came *before* it:

```cpp
bool RH_RF95::send(const uint8_t* data, uint8_t len) {
    waitPacketSent(); // Make sure we dont interrupt an outgoing message
    ...
}
```

Every node telemetry send and app-layer retransmit in `AppLayerAckSummary` mode (the mode
every shipping build uses) goes through this. Confirmed in the field via a second,
structurally distinct incident: a device log showed the radio service go silent for
~115 s immediately after three consecutive `retx_blocked` log lines (pure bookkeeping, no
radio I/O — so the hang has to be in whichever `send()` ran on the very next cycle), and
by the time it recovered the node had lost TDMA sync entirely, forcing a full
AWAKEN/TIME_SYNC handshake redo — a bigger operational hit than the ~115 s alone, since
that recovery path (`sendAwakenHandshake()`) is itself one of the *unfixed* Mechanism 3
call sites below.

**Fix:** `RadioHeadTdmaDriver::send()` got the identical bounded-wait treatment as
`acknowledge()` — waits after `sendto()` via `waitPacketSent(NetworkConfig::
kSendTxWaitMs)` (reuses `kBundleTxBudgetMs` = 340 ms, the largest existing per-slot TX
budget, since `send()` carries payloads up to a full `BUNDLE`), logging
`send_tx_timeout` on the rare give-up case rather than treating it as failure. Because
`RadioHeadTdmaDriver` is shared between node and base, the base's periodic `TIME_SYNC`
broadcast (which uses `send()`, not `sendToWait()`) picked up this protection for free.

### Mechanism 3 — `sendtoWait()`'s internal wait (open, not call-site-fixable)

`RHReliableDatagram::sendtoWait()` is not "send, then separately wait" from the caller's
side — it's a single, opaque vendored call that runs its own internal retry loop, and the
unbounded wait is *inside* that loop:

```cpp
bool RHReliableDatagram::sendtoWait(uint8_t* buf, uint8_t len, uint8_t address) {
    uint8_t thisSequenceNumber = ++_lastSequenceNumber;   // private state
    uint8_t retries = 0;
    while (retries++ <= _retries) {
        setHeaderId(thisSequenceNumber);
        setHeaderFlags(...);            // sets/clears the RETRY flag per attempt
        sendto(buf, len, address);
        waitPacketSent();                // <- unbounded; return value silently discarded
        if (address == RH_BROADCAST_ADDRESS) return true;
        if (retries > 1) _retransmissions++;   // private state
        // ... then a *separately bounded* wait for the ack reply (waitAvailableTimeout) ...
        // ... on a duplicate non-ack request seen while waiting: acknowledge(id, from) ...
    }
    return false;
}
```

The caller gets a single `bool` back once the *entire* loop finishes (success, or all
retries exhausted). There is no seam — no callback, no virtual hook, nowhere execution
returns to our code between one retry attempt and the next. If attempt #1's
`waitPacketSent()` hangs, there is no way for our code to intervene, because our code
isn't running — we're not the one driving that loop.

**Why we can't just reimplement it ourselves, even though all the low-level sending
primitives (`setHeaderId`, `setHeaderFlags`, `sendto`, the bounded
`waitPacketSent(timeout)`, `waitAvailableTimeout`, `recvfrom`) are public:** the
bookkeeping that makes the retry protocol *correct* is private, with no accessors at all:

```cpp
// RHReliableDatagram.h, private:
uint32_t _retransmissions;      // no getter
uint8_t  _lastSequenceNumber;   // no getter
uint8_t  _seenIds[256];         // no getter — per-sender last-seen ID, for duplicate rejection
```

(`_retries`/`setRetries()`/`retries()` and the ack-wait `_timeout` *are* publicly
configurable — it's specifically the *running* sequence number, retransmit counter, and
duplicate-tracking table that aren't.) A from-scratch reimplementation would mean
inventing our own versions of all three and getting the semantics exactly right —
sequence numbers the receiver's dedup logic expects, retry/backoff timing consistent with
what `acknowledge()`'s duplicate-detection assumes elsewhere, per-sender ID tracking that
doesn't drift from what the protocol actually needs. Get any of it subtly wrong and the
failure mode isn't "sometimes hangs" (loud, obvious — exactly what we've been chasing) —
it's "sometimes silently accepts the wrong ack" or "sometimes fails to dedupe a retry,"
which is far harder to detect and debug. That's a materially worse risk profile than
adding one bounded wait call, which is all Mechanisms 1 and 2 needed.

**Why patching the vendored `.cpp` directly isn't a clean shortcut either:** even a
targeted one-line swap (bounded `waitPacketSent(timeout)` in place of the unbounded call)
isn't actually a one-liner, because the surrounding loop currently *discards* that call's
return value and proceeds straight to the ack-wait phase regardless of whether the
transmission actually completed. A naive patch would burn the entire ack-timeout budget
waiting for a reply to a packet that may never have gone out — so the retry logic around
it would need adjusting too, i.e. more vendored-code surgery, not less. And structurally:
`.pio/libdeps/.../RadioHead` is a registry-fetched, PlatformIO-managed dependency, not a
copy checked into this repo — a hand-edit there can vanish on a clean build, a
`pio lib update`, or deleting `.pio` to fix a broken cache. Making a patch durable would
mean forking RadioHead into the repo itself (a separate, ongoing-maintenance decision —
manually tracking upstream fixes forever, or accepting a frozen fork), not something to
back into as a side effect of one bug fix.

**Why a hand-rolled "interrupt-based timeout wrapper" was also considered and rejected:**
in a single-threaded, non-preemptive-by-us architecture, there is no way to "cancel" a
blocked function call from outside using only more function calls — once
`sendtoWait()` is spinning, the entire CPU is inside that spin, and nothing else we wrote
gets to run until it returns on its own. The only thing that genuinely can preempt it is a
hardware interrupt. In principle you could arm a timer before calling `sendtoWait()` and
have its ISR forcibly change `_mode` so the stuck loop's condition goes false — but
`_mode` has no public setter, so the only legitimate lever is calling something like
`sleep()`, which does a real SPI transaction. Doing that from an interrupt context while
the radio might be physically mid-transmission is a genuine hazard (not fatal — it just
means deliberately corrupting whatever's currently on the air, same blast radius as the
sleep-race bug above — but not a "clean" mechanism either). Once you tally what this
actually requires — a dedicated timer, an ISR, reaching into driver internals via a
side-effecting call, accepting a transmission-corruption risk as the cost of recovery —
you've reinvented a hardware watchdog, just scoped to one function, with *more*
implementation risk than the real one: a WDT peripheral is purpose-built silicon for
exactly this pattern; a hand-rolled version is a narrower, more fragile copy of the same
idea. And the real WDT's reset clears *everything* (SPI bus state, the SX1276's own
internal state machine, whatever was left in its FIFO), where a surgical "unstick just
this one variable" approach leaves all of that in a state nobody's reasoned about.

**The six current call sites** (all confirmed directly against source, not from memory —
re-grep `\.sendToWait(` in `src/` and `include/` to re-verify if this list might be stale):

| Location | Function | Packet | Frequency in shipping (`AppLayerAckSummary`) config |
|---|---|---|---|
| `TdmaRadioService.cpp:183` | `sendAwakenHandshake()` (node) | `AWAKEN` | Once at boot, retried every 5s until synced — **also the recovery path after any TDMA-sync-losing hang**, i.e. exercised right when the node is already in a fragile state |
| `TdmaRadioService.cpp:220` | `sendImmediate(..., requireLinkAck=true)` (node), called from `SmartFiresNodeApp.cpp:475` | `CMD_ACK` | Only on `CMD_CALIBRATE`/`CMD_RESET` — operator-triggered, rare (zero occurrences in every log examined so far) |
| `TdmaRadioService.cpp:559` | `drainTxQueue()`'s link-ack retry branch (node) | `BUNDLE`/`STATUS` | Only reached when `telemetryUsesLinkAck()` is true, which is hard-coded false for `AppLayerAckSummary` — effectively dead code in production; only live in the diagnostics-only `StrictLinkAck` mode |
| `SmartFiresBaseApp.cpp:447` | `sendDirectTimeSync()` (base) | `TIME_SYNC` (direct) | Once per node `AWAKEN` received — same cadence as the node's AWAKEN handshake above, since it's the reply half of that exchange |
| `SmartFiresBaseApp.cpp:706` | `sendPendingCommand()` (base) | `CMD_CALIBRATE`/`CMD_RESET` | Operator-triggered, rare |
| `SmartFiresBaseApp.cpp:857` | `sendAckSummary()` (base) | `ACK_SUMMARY` | **The high-frequency one** — roughly every few seconds during ordinary operation (hundreds of times per multi-hour session in logs examined). By far the most-exercised instance of this exposure, and the strongest concrete argument for prioritizing Phase 2 |

If prioritizing which of these matters most: `sendAckSummary()` is what's actually getting
hammered in production. The AWAKEN/TIME_SYNC-direct pair is second — not because of raw
frequency, but because it's specifically the recovery path taken right after something
has *already* gone wrong (a node that just lost sync now can't even resync if this hangs
too). CMD_ACK, CMD dispatch, and the `StrictLinkAck`-only retry branch are all low-exposure
in the configuration actually shipped.

### Mechanism 4 — shared I2C bus (open, untouched)

SHT31, ICM-20948 (see note on current IMU wiring state below), and the PA1010D GPS all sit
on one `Wire` bus with no configured timeout and no bus-recovery sequence anywhere in the
firmware. A stalled slave (vibration, EMI, marginal pull-ups) can wedge
`Wire.endTransmission()`/`requestFrom()` indefinitely inside any sensor driver call. This
was the mechanism behind an earlier confirmed node hang (traced to
`SparkfunIcm20948Driver::read()` → `readDMPdataFromFIFO()` → ultimately
`SERCOM::readDataWIRE()`'s bare `while(sercom->I2CM.INTFLAG.bit.SB == 0) { }` spin, no
timeout, edge case where a slave never completes a byte handshake). No investigation or
fix has been attempted for this one — it remains fully open, and the same "no call-site
fix, no timeout primitive available" reasoning that applies to Mechanism 3 likely applies
here too, though it hasn't been analyzed with the same rigor.

**Note on current IMU wiring:** as of this writing, the ICM-20948 (`imu`/`imuCfg`/
`imuDriver` objects) is constructed in `main.cpp` but commented out of the active
`sensors[]` array (`// &imu`), so its DMP init and FIFO reads are **not currently
exercised** at runtime — the active sensor set is SHT31, GPS, SPS30, and wind (4
sensors). This is a mutable, easily-changed state, not a permanent architecture decision —
confirm current `sensors[]` contents before relying on this when tuning the boot-phase
timeout budget below, since re-enabling the IMU reintroduces its (unmeasured) DMP firmware
load time into the boot sequence.

**Relationship to [[reset-system]]:** `CMD_RESET` is an operator-initiated remote reset
that requires the node to be alive enough to receive a LoRa command and ACK it — exactly
the capability that's missing in every hang scenario this plan targets. The watchdog is a
last-resort *automatic* layer underneath `CMD_RESET`'s explicit layer, not a replacement
for it.

---

## Goals

- A hung node automatically reboots within a bounded, known time window — no manual
  intervention required in the field.
- The watchdog timeout is long enough that no legitimate firmware activity (boot-time
  I2C scan, sensor `begin()`, DMP init if the IMU is active, a full TDMA frame, GPS
  warm-up) ever trips it.
- After an auto-reboot, the node behaves exactly like any other boot: AWAKEN → TIME_SYNC
  → resume sensing. No state corruption risk since SAMD21 RAM is cleared on reset anyway,
  and (confirmed directly in source) `RadioHeadTdmaDriver::begin()` calls `resetRadio()`,
  which toggles the RFM95's actual hardware RST pin as part of normal init — so a
  WDT-triggered reboot hardware-resets the physical radio chip too, not just the SAMD21's
  software model of it. This matters specifically for Mechanisms 3 and 4: whatever bad
  state the SX1276 or an I2C slave was left in gets a genuine hardware reset, not just a
  software-level "forget about it and hope."
- The base station gets the same protection in a later phase — it has its own infinite
  retry loop on `begin()` failure (confirmed present in both the `LORA_NODE` and
  `LORA_BASE` branches of `main.cpp`) and the identical Mechanism 3 exposure via its own
  three `sendToWait()` calls.
- Whether the watchdog fired (vs. a normal power-on reset) is observable after the fact,
  so this doesn't quietly mask a hang that should be debugged at the root cause instead.

**Non-goal:** finding and fixing the *remaining* hangs (Mechanisms 3 and 4) at the call
site. That's not merely deferred — for Mechanism 3 it's been specifically analyzed and
concluded to be either much riskier (a from-scratch protocol reimplementation) or
structurally fragile (patching a non-vendored-in dependency) than accepting a coarser,
general-purpose safety net. Mechanisms 1 and 2 *were* fixed at the call site, precisely
because they turned out to be cheaply and safely fixable that way — this plan doesn't
need to touch those, they're mentioned for completeness and because the watchdog is still
useful defense-in-depth even where a bounded wait already exists.

---

## Design

### Hardware vs. software watchdog

Use the SAMD21's built-in hardware WDT, not a software/task-heartbeat watchdog — a
software watchdog (e.g., a second timer ISR checking a "last alive" timestamp) is itself
just more code that can hang or fail to fire under the exact conditions we're guarding
against.

**This chip already has one — no hardware changes needed.** The ATSAMD21G18 (the MCU on
the Adafruit Feather M0 boards used for both node and base) has a WDT as a standard
on-chip peripheral, clocked from `OSCULP32K` — an internal ~32.768 kHz ultra-low-power
oscillator that's independent of whatever clock the CPU core runs on for normal
operation. That independence is the entire point: even if the main clock system
misbehaves, or the CPU is completely wedged inside a stuck loop, the WDT's own oscillator
and counter keep running regardless, because they don't depend on anything the stuck CPU
does. Programmable timeout periods are a fixed set of options topping out around 16.384 s
without more elaborate window-mode tricks — this ceiling is a hardware property, not a
config choice, so pick whatever timeout strategy fits within it (see "Timeout budget"
below for why this plan splits into two phases rather than fighting that ceiling with one
value). When it fires, it asserts a genuine hardware reset — equivalent in effect to
pulling power and reconnecting it, not a soft/simulated reset.

**Library:** `Adafruit_SleepyDog` (confirmed: not currently in `platformio.ini`'s
`lib_deps` anywhere — this plan is still unimplemented). It wraps the SAMD21 WDT/clock-generator
setup that's otherwise several registers of boilerplate, and is the de facto standard for
Adafruit SAMD boards.

```cpp
#include <Adafruit_SleepyDog.h>
...
int actualMs = Watchdog.enable(8000);  // requests 8s, returns the closest achievable value
...
Watchdog.reset();  // "pet" the dog — call periodically, resets the countdown
```

### Where to pet the watchdog

`Watchdog.reset()` must be called from any code path that can legitimately run longer
than the configured timeout, not just once per `loop()` — otherwise normal startup work
trips it. Conveniently, `main.cpp`'s node branch already has a `Samd21RamMonitor`
diagnostic (`gRamMonitor`, unrelated purpose — it logs stack/heap-gap snapshots, purely
observational, no corrective action tied to it) checkpointed at almost exactly the
boundaries the watchdog needs too. Petting at the same points is a natural fit:

| Existing checkpoint (`main.cpp`, `LORA_NODE` branch) | Line | Pet here? |
|---|---|---|
| After `delay(5000)` / `Serial`/`Serial1` init (`while (!Serial1 && millis() < 3000)`) | ~294-302 | Yes — right after, before `gRamMonitor.begin()` or alongside it |
| `gRamMonitor.checkpoint("setup", "before_wire")` | 309 | Yes |
| After `Wire.begin()`/`analog.begin()` — `gRamMonitor.checkpoint("setup", "after_wire")` | 314 | Yes |
| After `scanI2C()` — `gRamMonitor.checkpoint("setup", "after_i2c_scan")` | 319 | Yes — this is the single riskiest boot step if the bus is already wedged at power-on |
| `gRamMonitor.checkpoint("app_begin", "before")` | 378 | Yes, immediately before `app.begin()` |
| `gRamMonitor.checkpoint("app_begin", "after")` | 387 | Yes — but see the infinite-retry-loop interaction below; do **not** pet inside the `while(true){delay(500);}` on `begin()` failure |
| `loop()`: `gRamMonitor.update(); app.update(); gRamMonitor.update(); delay(25);` | ~396-406 | Pet once per `loop()` call — e.g. right after `app.update()` returns, alongside the second `gRamMonitor.update()` |

Base station (`LORA_BASE` branch) doesn't currently have the RAM monitor, but has the
equivalent `baseApp.begin()` structure — pet at the analogous boot-step boundaries there
when Phase 2 lands.

**Do not pet the watchdog from inside any of the following** — that would defeat the
entire point, since these are exactly the calls the WDT needs to be able to catch:

- Any of the six `sendToWait()` call sites listed under Mechanism 3 above.
- Any SHT31/ICM-20948/GPS I2C driver call (Mechanism 4).
- `RadioHeadTdmaDriver::send()`/`acknowledge()` are technically now bounded (Mechanisms 1
  and 2), so petting around them wouldn't reintroduce the original hang risk — but keep
  them excluded anyway for consistency and defense-in-depth; a bounded wait is a strong
  mitigation, not a formal guarantee nothing can still go wrong there.

The existing pattern of one pet per `loop()` iteration (not scattered inside individual
radio/sensor calls) already satisfies this as long as the pet call site itself sits
outside all of the above — which the existing `gRamMonitor.update()` bracketing in
`loop()` already demonstrates the shape of.

### Timeout budget

Needs to be larger than the worst legitimate single `setup()`-to-first-`loop()` stretch,
and the worst legitimate single `loop()` iteration, with margin:

| Quantity | Approx. worst case | Source |
|---|---|---|
| `setup()` fixed delay | 5000 ms (`delay(5000)` at top of node `setup()`, confirmed `main.cpp:294`) | `main.cpp` |
| `setup()` serial wait | up to 3000 ms (`while (!Serial1 && millis() < 3000)`, `main.cpp:297`) | `main.cpp` |
| I2C scan (`scanI2C()`, `main.cpp:317`) | low hundreds of ms (127 addresses × ~1 ms each when nothing acks) — **unbounded if the bus is already wedged at boot**, which is exactly Mechanism 4 | `main.cpp` |
| ICM-20948 DMP firmware load (`initializeDMP()`) | not measured; SparkFun lib loads a ~14 KB firmware blob over I2C — **currently not exercised**, since `&imu` is commented out of the active `sensors[]` array as of this writing; re-check before assuming this doesn't need budget | `SparkfunIcm20948Driver.cpp` |
| One TDMA frame (steady state) | `NUM_SLOTS × slotWidthMs` = 4 × 900 ms = 3600 ms | `NetworkConfig.h` |
| One bundle TX | `kBundleTxBudgetMs` = 340 ms budgeted, plus retries | `NetworkConfig.h` |
| Bounded ACK/send waits (Mechanisms 1 & 2, now fixed) | `kAckTxWaitMs` = 90 ms, `kSendTxWaitMs` = 340 ms — each individually small, but note these are *per-call* bounds, not a bound on how many such calls can stack up in one `loop()` iteration | `NetworkConfig.h` |

Given the 5000 ms fixed boot delay alone already eats most of a single WDT window, the
plan splits the timeout by phase rather than picking one constant for both:

- **Boot phase:** arm the WDT at the *very top* of `setup()` with a longer timeout (e.g.
  16000 ms, the practical hardware ceiling) and pet it explicitly at each checkpoint
  listed above. This is the phase most exposed to Mechanism 4 (I2C wedged at boot, inside
  `scanI2C()` or a sensor's `begin()`). Re-arm to the shorter steady-state value at the
  end of `setup()`, immediately before entering `loop()`.
- **Steady-state phase:** ~8000 ms, comfortably more than double a full TDMA frame
  (3600 ms) and more than 10× a single bundle TX, so no normal `loop()` iteration is ever
  close to it, while still catching a real hang within single-digit seconds instead of
  the WDT's hardware ceiling.

These are starting values, not bench-verified — flag for tuning once real hardware timing
is measured (see Testing below), same as `rxWakeAheadMs` was for RX gating and
`kAckTxWaitMs`/`kSendTxWaitMs` are today.

### Interaction with existing infinite retry loops

Both node and base `setup()` have a deliberate infinite loop on `begin()` failure
(confirmed present in both branches of `main.cpp`):

```cpp
// main.cpp — LORA_NODE branch, ~line 380-385
if (!app.begin()) {
  LOG_INFO("boot", "smart_fires_app_status=%d", 1);
  while (true) { delay(500); }   // <-- currently unrecoverable without the WDT
}
```

```cpp
// main.cpp — LORA_BASE branch, analogous structure
if (!baseApp.begin()) {
  while (true) { delay(500); }
}
```

This is presumably intentional (don't proceed with a half-initialized app), but today it
is just as unrecoverable as the hangs this plan targets. Once the WDT is armed before
this point, that loop self-resolves: the board reboots and retries `begin()` from scratch
after one WDT period. No code change needed in the loop itself — just confirm the WDT is
armed and petted right up until entry into this loop, then deliberately **not** petted
inside it, so it expires and reboots as intended.

### Interaction with `NVIC_SystemReset()` (existing hard CMD_RESET path)

Both apps already call `NVIC_SystemReset()` directly on a hard-reset command — confirmed
at `SmartFiresNodeApp.cpp:438` and `SmartFiresBaseApp.cpp:1032`. This is unaffected by
adding a WDT — `NVIC_SystemReset()` is a normal MCU reset, and the WDT config is
reinitialized from scratch on the next boot exactly like a power-on reset. No conflict,
no special-casing needed.

### Relationship to the existing `Samd21RamMonitor`

`platform/Samd21RamMonitor.cpp` (already present, node-only as of this writing) is a
purely diagnostic stack/heap-gap logger — it samples the stack pointer and heap-break
address, logs a snapshot at checkpoints and periodically, and exposes a `critical()`
accessor that (confirmed via grep) is **not currently checked anywhere** — it takes no
corrective action on its own. This is an orthogonal concern to the watchdog: it detects a
*different* failure mode (stack/heap collision from overflow or unbounded heap growth),
not a hung blocking call. No interaction or conflict — the watchdog doesn't need to touch
it, and it's mentioned here only so a fresh implementer doesn't mistake `gRamMonitor`
calls in `setup()`/`loop()` for something related to petting logic (beyond sharing
convenient checkpoint locations, per "Where to pet" above).

### Diagnosing whether the WDT fired

After a WDT-triggered reset, the SAMD21's `RSTC->RCAUSE` register has the `WDT` bit set
(distinct from `POR`/`BOD33`/`SYST`/`EXT`). Log this once at the top of `setup()`, before
it's cleared by anything else:

```cpp
const uint8_t resetCause = RSTC->RCAUSE.reg;
LOG_WARN("boot", "reset_cause=0x%02X wdt=%u",
         resetCause, (resetCause & RSTC_RCAUSE_WDT) ? 1 : 0);
```

This is important so a WDT-recovered hang is still visible after the fact (in the debug
log / `PKT_DEBUG_LOG` stream to the Jetson) instead of silently looking like a routine
power cycle. Consider surfacing a lifetime "WDT reset count" the same way `retx_total`/
`fail_total` already ride along in `STATUS` — open question, not required for Phase 1.

---

## Implementation Phases

| Phase | Scope | Files | Reflash? |
|---|---|---|---|
| 1 | Add `Adafruit_SleepyDog` to `lib_deps`; arm + pet WDT in node `setup()`/`loop()` at the checkpoints listed above; log `RCAUSE` on boot | `platformio.ini`, `main.cpp` (`LORA_NODE` branch) | All nodes |
| 2 | Same treatment for the base station (`LORA_BASE` branch) — base's own infinite `begin()`-failure loop, plus its three Mechanism-3 `sendToWait()` call sites (`sendDirectTimeSync()`, `sendAckSummary()`, `sendPendingCommand()`), get the same coverage | `main.cpp` (`LORA_BASE` branch) | Base only |
| 3 (optional) | Surface WDT-reset count to the Jetson (new STATUS/debug field or just a debug-log line), so field WDT trips are visible without a serial connection | `BinaryPacket.h`, `PacketHandler`, `packet.py` | All, + Jetson |

Phase 1 ships first and independently — it's the side with the actual field-confirmed
hangs (Mechanisms 1 and 2's field incidents both happened on the node). Phase 2 (base) has
sometimes been deferred as "the base hasn't been reported hanging yet" — that reasoning no
longer holds as a reason for low urgency: `sendAckSummary()` is a specifically identified,
currently-unmitigated, high-frequency (~every few seconds) instance of Mechanism 3 running
constantly on the base, not a generic "probably also exposed" concern. Unlike Mechanisms 1
and 2, there is no equivalent "disable the automatic path and hand-roll a bounded
replacement" fix available for the base's three `sendToWait()` calls — the base has to
actually transmit `ACK_SUMMARY`/`TIME_SYNC`-direct/commands to do its job, and (per the
Mechanism 3 analysis above) reimplementing RadioHead's retry protocol from scratch carries
real correctness risk that a watchdog entirely avoids. The watchdog is the practical
mitigation for Phase 2, not a call-site change — treat Phase 2 as no lower priority than
Phase 1 got, just sequenced second for shipping-incrementally reasons, not risk reasons.

`feather_m0_sensor_probe`, `feather_m0_lora_sniffer`, and `native` build environments are
out of scope — they're bring-up/diagnostic/test tools, not field-deployed firmware.

---

## Testing

No native/`pio test -e native` coverage is possible for the WDT itself — it's a real
hardware peripheral with no software fake, and arming a real WDT inside the desktop test
binary would be meaningless. Verification has to happen on actual Feather M0 hardware:

1. **Negative test (no false positives):** flash with the WDT armed, run for a full boot
   cycle + several TDMA frames with debug logging on, confirm no unexpected reboot and
   `RCAUSE` shows `POR` only on the one intentional power-up.
2. **Positive test (it actually catches a hang):** temporarily insert an artificial
   infinite loop (e.g., a `while(true);` patched into a throwaway build) somewhere in
   `loop()`, confirm the board reboots within the configured steady-state timeout and
   `RCAUSE` shows `WDT` on the next boot. Revert before shipping.
3. **Boot-phase test:** temporarily stall the I2C bus at boot (disconnect/short a sensor)
   and confirm the *boot-phase* timeout (not the shorter steady-state one) is what's
   active during `scanI2C()`/sensor `begin()`, and that the board still recovers rather
   than reboot-looping faster than the I2C bus has a chance to be the actual problem.
4. **Mechanism 3 regression test, once Phase 1/2 both ship:** if feasible, deliberately
   induce a missed-DIO0-interrupt-like condition (e.g., disconnect the antenna or induce
   RF interference during a `sendAckSummary()` window) and confirm the base recovers via
   WDT reset within the configured steady-state window, rather than staying silent
   indefinitely the way the node did in the original field incident.

Per `SmartFires_IoT/CLAUDE.md`'s guardrails, these are commands for the user to run
(`pio run -e feather_m0_lora_node --target upload`, `pio device monitor`) — not something
to execute automatically in this environment.

---

## Open Questions

- **Exact boot-phase vs. steady-state timeout values** — the budget table above is a
  starting estimate, not bench-measured. DMP init time in particular is unverified, and
  currently moot if the IMU stays disabled — re-confirm the `sensors[]` array's contents
  before finalizing the boot-phase timeout.
- **Should `Watchdog.reset()` calls be added inside `DutyCycleController`/sensor
  `service()` loops too**, in case a sensor's own `service()` (not just `begin()`) can run
  long? Current read of `serviceAllSensors()` shows non-blocking polling only (GPS reads
  ≤16 chars per call), so likely unnecessary, but worth confirming once Phase 1 is
  bench-tested against real timing.
- **Phase 3's WDT-reset-count telemetry** — worth its own design pass (packet format
  changes) once it's clear from field data whether WDT trips are rare enough not to need
  remote visibility, or frequent enough that "how often is this actually firing" becomes
  its own operational question.
- **Mechanism 4 (I2C bus) has not received the same depth of analysis as Mechanism 3** —
  it's plausible a bus-recovery sequence (toggling SCL up to 9 times while watching SDA,
  then a STOP condition) could mitigate at least the "genuinely wedged bus" sub-case
  without needing the watchdog at all, the way Mechanisms 1 and 2 turned out to be
  call-site-fixable. This hasn't been investigated with the same rigor and shouldn't be
  assumed impossible — a bus-recovery routine is a smaller, lower-risk change than
  anything considered for Mechanism 3, since it doesn't touch RadioHead's protocol state
  at all.
