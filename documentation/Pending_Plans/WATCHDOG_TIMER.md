---
name: watchdog-timer
description: Plan to add a hardware watchdog to node firmware (Phase 1) and the base station firmware (Phase 2) so an unrecoverable hang self-heals via reboot instead of requiring a manual power cycle.
category: plan-pending
status: draft
related_docs:
  - tdma-protocol
  - reset-system
  - tunable-parameters
  - packet-reliability
---

# Watchdog Timer

## Background

Field reports: nodes occasionally stop responding entirely (no telemetry, no AWAKEN
retries, no debug log activity) and only recover after a manual reset/power cycle. A
code scan turned up two concrete hang candidates, neither of which has any internal
timeout:

1. **`RH_RF95::send()`** (vendored RadioHead, `.pio/libdeps/.../RadioHead/RH_RF95.cpp:334`)
   opens with the no-argument `waitPacketSent()`
   (`RHGenericDriver.cpp:58-61`):
   ```cpp
   bool RHGenericDriver::waitPacketSent()
   {
       while (_mode == RHModeTx)
           YIELD;          // no timeout — depends on the TX-done IRQ firing
       return true;
   }
   ```
   `_mode` only leaves `RHModeTx` from the DIO0 interrupt handler. Every node send
   (`AWAKEN`, `STATUS`, `BUNDLE`, `CMD_ACK`) funnels through this call. If that IRQ
   edge is ever missed (line glitch, brownout, ESD on an outdoor harness), the call
   never returns — the MCU spins inside RadioHead forever, with no further log output.

   **Update 2026-07-10 — confirmed and partially mitigated.** A device log caught this
   exact hang: the node's auto-ACK-on-receive of an `ACK_SUMMARY` packet (routed through
   RadioHead's `RHReliableDatagram::acknowledge()`) called this same no-timeout
   `waitPacketSent()` and hung for ~115 s before recovering on its own. The node-side
   trigger (auto-ACK on receive) is now fixed — see
   [[packet-reliability]]'s "`waitPacketSent()` Has No Timeout" section for the full
   mechanism and fix. **The base station's exposure is untouched by that fix and remains
   fully open**: `SmartFiresBaseApp`'s three `sendToWait()` call sites
   (`sendDirectTimeSync()`, `sendAckSummary()`, `sendPendingCommand()`) each call this
   identical no-timeout wait for their *own* outbound transmission, independent of
   anything the node does. This was previously a theoretical "same class of exposure";
   it's now a specifically analyzed, currently-live risk on hardware that transmits
   `ACK_SUMMARY` roughly every few seconds during normal operation. See Phase 2 below.

   **Update 2026-07-13 — the fix's first version had its own bug, now corrected.** The
   initial mitigation had the node's replacement `acknowledge()` return immediately after
   `sendto()`, with no wait of any kind — safe from the hang, but with nothing guaranteeing
   the ACK had finished transmitting before something else (`TdmaRadioService::
   updateRxPower()`'s `sleep()` call, which has no in-flight-TX guard) could act on the
   radio next and silently abort it mid-send. This surfaced as the base station
   retransmitting `ACK_SUMMARY` far more than expected. Fixed by using RadioHead's
   *bounded* `waitPacketSent(timeout)` overload instead of no wait at all — see
   [[packet-reliability]] for the full before/after.
2. **Shared I2C bus** — SHT31, ICM-20948, and the PA1010D GPS all sit on one `Wire`
   bus with no configured timeout and no bus-recovery sequence anywhere in the
   firmware. A stalled slave (vibration, EMI, marginal pull-ups) can wedge
   `Wire.endTransmission()`/`requestFrom()` indefinitely inside any sensor driver call.

Both are inside vendored library code or core `Wire` internals — neither is cheaply
patchable without forking a dependency, and even a patched version only covers the
one hang found *today*. A watchdog timer is the general-purpose fix: it doesn't care
which subsystem wedges, and it converts "dead until someone drives out and power-cycles
it" into "dead for at most one WDT period."

**Relationship to [[reset-system]]:** `CMD_RESET` is an operator-initiated remote reset
that requires the node to be alive enough to receive a LoRa command and ACK it — exactly
the capability that's missing in the hang scenario this plan targets. The watchdog is a
last-resort *automatic* layer underneath `CMD_RESET`'s explicit layer, not a replacement
for it.

---

## Goals

- A hung node automatically reboots within a bounded, known time window — no manual
  intervention required in the field.
- The watchdog timeout is long enough that no legitimate firmware activity (boot-time
  I2C scan, sensor `begin()`, DMP init, a full TDMA frame, GPS warm-up) ever trips it.
- After an auto-reboot, the node behaves exactly like any other boot: AWAKEN → TIME_SYNC
  → resume sensing. No state corruption risk since SAMD21 RAM is cleared on reset anyway.
- The base station gets the same protection in a later phase — it has its own infinite
  retry loops (`begin()` failure, LoRa radio bad state per [[reset-system]]'s Background
  item 2) that a watchdog would also catch.
- Whether the watchdog fired (vs. a normal power-on reset) is observable after the fact,
  so this doesn't quietly mask a hang that should be debugged at the root cause instead.

**Non-goal:** finding and fixing the underlying hang(s) above. That's tracked separately
(see prior diagnostic findings) — this plan is the safety net, not the cure.

---

## Design

### Hardware vs. software watchdog

Use the SAMD21's built-in hardware WDT, not a software/task-heartbeat watchdog — a
software watchdog (e.g., a second timer ISR checking a "last alive" timestamp) is itself
just more code that can hang or fail to fire under the exact conditions we're guarding
against. The hardware WDT is a separate clock domain that resets the MCU regardless of
what the CPU core is doing.

**Library:** `Adafruit_SleepyDog` (not currently a `lib_deps` entry — needs adding to
`platformio.ini`). It wraps the SAMD21 WDT/clock-generator setup that's otherwise
several registers of boilerplate, and is already the de facto standard for Adafruit
SAMD boards (used elsewhere in the Adafruit ecosystem these sensors come from).

```cpp
#include <Adafruit_SleepyDog.h>
...
int actualMs = Watchdog.enable(8000);  // requests 8s, returns the closest achievable value
...
Watchdog.reset();  // "pet" the dog — call periodically, resets the countdown
```

`Watchdog.enable()` on SAMD21 tops out around **16 seconds** without window-mode tricks
(the WDT clock generator's max prescaler). Confirm whatever timeout we pick is comfortably
under that ceiling — see the budget table below.

### Where to pet the watchdog

`Watchdog.reset()` must be called from any code path that can legitimately run longer
than the configured timeout, not just once per `loop()` — otherwise normal startup work
trips it.

| Phase | Call sites that need a pet | Why |
|---|---|---|
| `setup()` | After `Serial`/`Serial1` wait loops, after `Wire.begin()`/`scanI2C()`, after each sensor `begin()` (SHT31, GPS, IMU DMP init, SPS30) | Boot-time init is the single longest contiguous stretch of firmware execution — DMP firmware load alone is non-trivial |
| `loop()` | Once per `app.update()` iteration (top or bottom of `loop()`) | Normal steady-state — one TDMA frame's worth of work happens across many `loop()` iterations, not one |

Do **not** pet the watchdog from inside `RadioHeadTdmaDriver::send()`/`sendToWait()` or
any sensor driver call — that would defeat the entire point (those are exactly the calls
we need the WDT to catch if they hang).

### Timeout budget

Needs to be larger than the worst legitimate single `setup()`-to-first-`loop()` stretch,
and the worst legitimate single `loop()` iteration, with margin:

| Quantity | Approx. worst case | Source |
|---|---|---|
| `setup()` serial wait | up to 3000 ms (`while (!Serial1 && millis() < 3000)`) | `main.cpp` |
| `setup()` fixed delay | 5000 ms (`delay(5000)` at top of node `setup()`) | `main.cpp` |
| I2C scan (`scanI2C()`) | low hundreds of ms (127 addresses × ~1 ms each when nothing acks) — **unbounded if the bus is already wedged at boot**, which is exactly the failure mode this plan defends against | `main.cpp` |
| ICM-20948 DMP firmware load (`initializeDMP()`) | not measured; SparkFun lib loads a ~14 KB firmware blob over I2C | `SparkfunIcm20948Driver.cpp` |
| One TDMA frame (steady state) | `NUM_SLOTS × slotWidthMs` = 4 × 900 ms = 3600 ms | `NetworkConfig.h` |
| One bundle TX | 340 ms budgeted, plus retries | `NetworkConfig.h` |

Given the 5000 ms fixed boot delay alone already eats most of a single WDT window, the
plan splits the timeout by phase rather than picking one constant for both:

- **Boot phase:** arm the WDT at the *very top* of `setup()` with a longer timeout (e.g.
  16000 ms, the practical ceiling) and pet it explicitly after each major boot step listed
  above. This is the phase most exposed to the I2C-scan-hangs-at-boot scenario.
  Re-arm to the shorter steady-state value at the end of `setup()`, immediately before
  entering `loop()`.
- **Steady-state phase:** ~8000 ms, comfortably more than double a full TDMA frame
  (3600 ms) and more than 10× a single bundle TX, so no normal `loop()` iteration is
  ever close to it, while still catching a real hang within single-digit seconds instead
  of the WDT's hardware ceiling.

These are starting values, not bench-verified — flag for tuning once real hardware
timing is measured (see Testing below), same as `rxWakeAheadMs` was in
[[project_lora_rx_gating]].

### Interaction with existing infinite retry loops

Both node and base `setup()` already have a deliberate infinite loop on `begin()`
failure:

```cpp
// main.cpp, both LORA_NODE and LORA_BASE setup()
if (!app.begin()) {
  while (true) { delay(500); }   // <-- currently unrecoverable without the WDT
}
```

This is presumably intentional (don't proceed with a half-initialized app), but today it
is just as unrecoverable as the hangs this plan targets. Once the WDT is armed before
this point, that loop self-resolves: the board reboots and retries `begin()` from
scratch after one WDT period. No code change needed in the loop itself — just confirm
the WDT is armed and petted right up until entry into this loop, then deliberately
**not** petted inside it, so it expires and reboots as intended.

### Interaction with `NVIC_SystemReset()` (existing hard CMD_RESET path)

`SmartFiresNodeApp.cpp`'s hard-reset path already calls `NVIC_SystemReset()` directly.
This is unaffected by adding a WDT — `NVIC_SystemReset()` is a normal MCU reset, the WDT
config is reinitialized from scratch on the next boot exactly like a power-on reset.
No conflict, no special-casing needed.

### Diagnosing whether the WDT fired

After a WDT-triggered reset, the SAMD21's `RSTC->RCAUSE` register has the `WDT` bit set
(distinct from `POR`/`BOD33`/`SYST`/`EXT`). Log this once at the top of `setup()`,
before it's cleared by anything else, e.g.:

```cpp
const uint8_t resetCause = RSTC->RCAUSE.reg;
LOG_WARN("boot", "reset_cause=0x%02X wdt=%u",
         resetCause, (resetCause & RSTC_RCAUSE_WDT) ? 1 : 0);
```

This is important so a WDT-recovered hang is still visible after the fact (in the debug
log / `PKT_DEBUG_LOG` stream to the Jetson) instead of silently looking like a routine
power cycle. Consider surfacing a lifetime "WDT reset count" the same way `retx_total`/
`fail_total` already ride along in `STATUS` ([[project_overview]]) — open question, not
required for Phase 1.

---

## Implementation Phases

| Phase | Scope | Files | Reflash? |
|---|---|---|---|
| 1 | Add `Adafruit_SleepyDog` to `lib_deps`; arm + pet WDT in node `setup()`/`loop()`; log `RCAUSE` on boot | `platformio.ini`, `main.cpp` (`LORA_NODE` branch) | All nodes |
| 2 | Same treatment for the base station (`LORA_BASE` branch) — base's own infinite `begin()`-failure loop and any future radio-reinit hang get the same coverage | `main.cpp` (`LORA_BASE` branch) | Base only |
| 3 (optional) | Surface WDT-reset count to the Jetson (new STATUS/debug field or just a debug-log line), so field WDT trips are visible without a serial connection | `BinaryPacket.h`, `PacketHandler`, `packet.py` | All, + Jetson |

Phase 1 ships first and independently — it's the side with the actual field complaint.
Phase 2 (base) is explicitly deferred per the user's request, since the base has not been
reported hanging yet, but should get the same treatment once Phase 1 is proven out, since
it has the same class of exposure (its own infinite retry loop, plus a LoRa radio that can
enter a bad state per [[reset-system]]'s Background item 2) — **and, as of the 2026-07-10
update above, a specifically identified, currently-unmitigated `waitPacketSent()` hang risk
on its own `ACK_SUMMARY`/`TIME_SYNC`-direct/CMD `sendToWait()` calls**, not just a
generic "probably also exposed" concern. Unlike item 1's node-side trigger, there is no
equivalent "disable and hand-roll a non-blocking version" fix available on the base for
these three call sites, since the base has to actually transmit them to do its job — the
watchdog is the only practical mitigation here.

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

Per `SmartFires_IoT/CLAUDE.md`'s guardrails, these are commands for the user to run
(`pio run -e feather_m0_lora_node --target upload`, `pio device monitor`) — not something
to execute automatically in this environment.

---

## Open Questions

- **Exact boot-phase vs. steady-state timeout values** — the budget table above is a
  starting estimate, not bench-measured. DMP init time in particular is unverified.
- **Should `Watchdog.reset()` calls be added inside `DutyCycleController`/sensor `service()`
  loops too**, in case a sensor's own `service()` (not just `begin()`) can run long? Current
  read of `serviceAllSensors()` shows non-blocking polling only (GPS reads ≤16 chars per
  call, IMU reads one FIFO frame), so likely unnecessary, but worth confirming once Phase 1
  is bench-tested against real timing.
- **Phase 3's WDT-reset-count telemetry** — worth its own design pass (packet format
  changes) once it's clear from field data whether WDT trips are rare enough not to need
  remote visibility, or frequent enough that "how often is this actually firing" becomes
  its own operational question.
