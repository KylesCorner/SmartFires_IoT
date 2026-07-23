---
name: reset-reason-diagnostics
description: Plan to report node reset cause (WDT/brownout/power-on) and a hang-zone breadcrumb through the AWAKEN packet, so watchdog reboots can be attributed to the I2C-stall vs RadioHead-hang candidates.
category: plan-pending
status: implemented-phase-1-2
related_docs:
  - watchdog-timer
  - reset-system
  - packet-reliability
  - uart-jetson-bridge
---

# Reset Reason Diagnostics

## Implementation status (Phase 1 + Phase 2 shipped)

Both phases below are implemented as of this build; Phase 3 (WDT early-warning PC
capture) remains deferred. Touchpoints as built:

- **Wire:** `AwakenPayload` grew 4 → 6 bytes (`reset_cause`, `hang_zone`); AWAKEN
  LoRa frame 9 → 11 bytes. `decodeAwaken` (firmware) and `decode_awaken`
  (`packet.py`) are length-adaptive — a legacy 9-byte node still decodes, new
  fields → 0/None.
- **Breadcrumb:** `include/platform/ResetDiagnostics.h` + `src/platform/
  ResetDiagnostics.cpp` hold the `.noinit` `ResetBreadcrumb` (magic + zone +
  ~zone + boot_count), `harvest()`, `markZone()`, and a `ZoneScope` RAII guard.
- **Linker:** `ldscripts/flash_with_bootloader_noinit.ld` adds a `.noinit
  (NOLOAD)` section; node envs point `board_build.ldscript` at it and define
  `-DSMARTFIRES_RESET_DIAG`. Base/native are unchanged (plain zeroed global,
  stock ldscript).
- **Harvest + reporting:** `main.cpp` (node) calls `ResetDiagnostics::harvest()`
  right after reading `PM->RCAUSE`, marks `ZONE_BOOT` → `ZONE_LOOP_IDLE`;
  `SmartFiresNodeApp::sendAwakenHandshake()` populates the payload.
- **Zones instrumented:** `ZONE_RADIO_TX` (RadioHeadTdmaDriver send/sendToWait/
  acknowledge), `ZONE_I2C_SHT31`, `ZONE_I2C_GPS`, `ZONE_I2C_IMU`,
  `ZONE_UART_SPS30`.
- **Edge:** `ingest_service.py` writes `reset_cause`/`reset_cause_names`/
  `hang_zone`/`hang_zone_name` into each `awaken` row of `status.jsonl` and
  `telemetry.csv` (the latter was silently crashing the entire ingest loop on
  every AWAKEN until `csv_logger.CSV_COLUMNS` was updated to include the four
  new fields — `csv.DictWriter` defaults to `extrasaction="raise"`).
- **Dashboard restart-bucketing:** `SessionTelemetryCache.awaken_events()` +
  `GET /api/awaken_events` surface every AWAKEN this session (timestamp, node,
  reset cause, hang zone, seq, RSSI) in a "Node Reboot Events" table on the
  Map & History page (`map_history.html`/`map_history_page.js`), colour-flagging
  WDT-caused reboots. Current-session only (matches the rest of the dashboard);
  legacy pre-diagnostics AWAKEN frames show "—".

Remaining before moving to `Completed_Plans/`: hardware validation (induced-hang
test per the Validation section) and the SOFTWARE_DESIGN.md / TDMA_PROTOCOL.md /
BANDWIDTH_SCALING.md wire-table updates.

## Background

The overnight run of 2026-07-14 (`analysis and scripts/data/2026-07-14_233947/RUN_REPORT.md`)
showed 15 node reboots in 16.5 h, confirmed by status-packet `seq` + lifetime `retx_total`
both zeroing at each event. The watchdog (see `Completed_Plans/WATCHDOG_TIMER.md`) is the
presumed trigger, but nothing on the wire proves it, and nothing distinguishes the two
root-cause candidates:

1. **Shared I2C bus stall** — a sensor transaction hangs the blocking Wire library.
   2 of the 15 reboots were preceded by heavy sensor-glitch storms consistent with this.
2. **RadioHead TX-done hang** — `waitPacketSent()` blocks forever on a missed interrupt
   (the same bug class that motivated disabling autoAck; see `PACKET_RELIABILITY.md`).
   The other 13 reboots had no preceding sensor glitches and occurred on a marginal link
   (RSSI −90 to −96 dBm), consistent with this.

Today the SAMD21 reset cause is read at boot (`main.cpp` — `PM->RCAUSE.reg`) but only
logged to the node's **local serial**, which is unattached in field deployments. And
`RCAUSE` alone can only say *that* the WDT fired, never *what the code was doing* when
it hung.

## Goals

- Every node boot reports its hardware reset cause (`RCAUSE`: WDT / BOD33 brownout /
  power-on / external) to the Jetson dashboard.
- WDT-triggered reboots additionally report which blocking region the firmware was in
  when it hung ("hang zone"), differentiating I2C-stall from RadioHead-hang.
- Rollout must not flood the edge decoder with length failures during version skew.

## Non-goals

- Fixing the hangs themselves (that's follow-up work once attribution is known).
- Base-station reset diagnostics (base has no AWAKEN; defer, mirroring watchdog Phase 2).
- Exact program-counter capture via the WDT early-warning interrupt (Phase 3, deferred —
  requires bypassing Adafruit_SleepyDog and driving WDT registers directly).

---

## Phase 1 — `reset_cause` through AWAKEN

Smallest useful step: distinguishes WDT from true brownout (BOD33) and power cycles with
one byte the node already reads.

### Wire change

`AwakenPayload` grows from 4 to 6 bytes; AWAKEN LoRa payload 9 → 11 bytes
(UART frame data len 10 → 12):

```c
struct __attribute__((packed)) AwakenPayload {
    uint32_t uid_hash;
    uint8_t  reset_cause;   // raw PM->RCAUSE.reg from this boot
    uint8_t  hang_zone;     // Phase 2; 0 = ZONE_UNKNOWN until then
};
```

### Touchpoints

| Piece | Location | Change |
|---|---|---|
| Payload struct, `static_assert`, `kAwakenLoRaSize`, header comment table | `platformio/include/telemetry/BinaryPacket.h` | 4 → 6 byte payload; size 9 → 11 |
| Stash `RCAUSE` for later use | `platformio/src/main.cpp` (node section) | already read into a local; move into `ResetDiagnostics` (Phase 2 header) or pass via `SmartFiresNodeApp::Config` |
| Populate new fields | `SmartFiresNodeApp::sendAwakenHandshake()` (`SmartFiresNodeApp.cpp`) | set `reset_cause`, `hang_zone` |
| Base decode | `BinaryPacket::decodeAwaken()` | make **length-adaptive**: accept legacy 9-byte payloads, defaulting new fields to 0 — so base/node flash order doesn't matter |
| Base relay | `SmartFiresBaseApp.cpp` | none beyond recompile (payload relayed opaquely; frame len byte follows automatically) |
| Edge decode | `edge/edge-receiver/src/smartfires_edge/packet.py` | `AWAKEN_PAYLOAD_FMT = "<IBB"`; `decode_awaken()` length-adaptive (accept old 9-byte payloads, fields → `None`) |
| Edge surfacing | `ingest_service.py`, `live_state.py`, `web/` | add `reset_cause`/`hang_zone` to the awaken record written to `status.jsonl` and the dashboard's restart indicator (bucket restarts by cause) |

### Rollout order (avoids length-failure flood)

1. Edge first — length-adaptive decode accepts both sizes.
2. Base and node Feathers together (length-adaptive `decodeAwaken` makes this forgiving,
   but they share `BinaryPacket.h` so flash both anyway).

---

## Phase 2 — hang-zone breadcrumb in noinit RAM

SAMD21 SRAM retains contents across a WDT/system reset (not across power loss) — the
same property the bootloader's double-tap magic relies on. A small struct that crt0
does **not** zero holds a "zone" byte written before each blocking region:

```c
// include/platform/ResetDiagnostics.h
enum HangZone : uint8_t {
  ZONE_UNKNOWN = 0,   // breadcrumb invalid or WDT fired outside any marked region
  ZONE_BOOT,          // setup()/begin() phase
  ZONE_RADIO_TX,      // RadioHeadTdmaDriver send/waitPacketSent path
  ZONE_I2C_SHT31,
  ZONE_I2C_GPS,       // PA1010D
  ZONE_I2C_IMU,       // ICM-20948 / DMP
  ZONE_UART_SPS30,
  ZONE_LOOP_IDLE,     // marked region exited normally
};

struct ResetBreadcrumb {   // lives in .noinit
  uint32_t magic;          // validity guard — RAM is garbage on cold power-up
  uint8_t  zone;
  uint8_t  zone_inv;       // ~zone, cheap integrity check
  uint16_t boot_count;
};
```

### Linker script prerequisite

The stock Adafruit `feather_m0` linker script (`flash_with_bootloader.ld`) has **no
`.noinit` output section** — `__attribute__((section(".noinit")))` would be silently
placed as an orphan. Add a repo-local copy at `platformio/ldscripts/
flash_with_bootloader_noinit.ld` with a `.noinit (NOLOAD)` section between `.bss` and
the heap-start symbols (`end`/`__end__`), and point the node environments at it via
`board_build.ldscript` in `platformio.ini`. Base env doesn't need it (no breadcrumb yet).

### Boot-time harvest (in `setup()`, right after reading `RCAUSE`)

```
wdt_fired   = RCAUSE & PM_RCAUSE_WDT
crumb_valid = (magic == kMagic) && (zone_inv == ~zone)
hang_zone   = (wdt_fired && crumb_valid) ? zone : ZONE_UNKNOWN
```

Then re-initialize the breadcrumb (write magic, zone = ZONE_BOOT, boot_count++) and
carry `hang_zone` into the AWAKEN payload. On non-WDT resets the harvested zone is
deliberately discarded — a brownout mid-I2C-read is not a hang.

### Instrumentation points

Single volatile byte store on entry/exit of each blocking region — no measurable
overhead. Start at the driver level (the actual blocking call sites):

| Zone | Where |
|---|---|
| `ZONE_RADIO_TX` | `RadioHeadTdmaDriver.cpp` around send/`waitPacketSent()`/ACK-wait paths |
| `ZONE_I2C_SHT31` | `AdafruitSht31Driver.cpp` bus transactions |
| `ZONE_I2C_GPS` | `AdafruitGpsDriver.cpp` bus transactions |
| `ZONE_I2C_IMU` | `SparkfunIcm20948Driver.cpp` DMP/bus transactions |
| `ZONE_UART_SPS30` | `SensirionUartSps30Driver.cpp` read/write waits |
| `ZONE_BOOT` | set at top of `setup()`, cleared to `ZONE_LOOP_IDLE` entering `loop()` |

Exit restores `ZONE_LOOP_IDLE` rather than clearing to 0, so `ZONE_UNKNOWN`
unambiguously means "breadcrumb invalid," and `ZONE_LOOP_IDLE` means "WDT fired in
unmarked code" — itself a useful signal that both prime suspects are innocent.

---

## Phase 3 (deferred) — WDT early-warning PC capture

If zone granularity proves insufficient: enable the SAMD21 WDT early-warning interrupt,
and in the ISR copy the stacked return address (exact hung instruction) into the
breadcrumb for lookup in the build's `.map` file. Requires abandoning Adafruit_SleepyDog
for direct WDT register control on the node. Hold in reserve.

---

## Validation

- **Native unit tests**: AWAKEN encode/decode at both 9- and 11-byte sizes (node, and a
  mirrored case in edge tests if any exist for `packet.py`); breadcrumb harvest logic
  (valid/invalid magic × WDT/non-WDT cause) with the struct in ordinary test memory.
- **Hardware, induced hang**: temporary debug hook (build-flag-gated) that deliberately
  spins inside a chosen zone; verify the WDT fires and the next AWAKEN reports
  `reset_cause` with the WDT bit and the correct zone. Repeat for a radio zone and an
  I2C zone. (A `CMD_CALIBRATE`-style triggered variant would allow remote testing, but
  a build flag is enough for bench validation.)
- **Hardware, cold boot**: power-cycle → AWAKEN shows POR cause and `ZONE_UNKNOWN`
  (garbage RAM rejected by magic check).
- **Hardware, regression**: double-tap bootloader entry still works with the custom
  linker script; RAM watermark (`Samd21RamMonitor`) unchanged apart from the 8-byte
  breadcrumb.
- **Field**: rerun the overnight test; dashboard restart counter now buckets by
  cause/zone. Expected outcome per the 2026-07-14 run: mostly WDT+`ZONE_RADIO_TX`, a
  minority WDT+I2C zones.

## Docs to update on completion

- `CLAUDE.md` and `documentation/SOFTWARE_DESIGN.md` wire-protocol tables (AWAKEN 9 → 11
  bytes, new fields).
- `Current_Architecture/TDMA_PROTOCOL.md` (boot handshake payload),
  `BANDWIDTH_SCALING.md` (AWAKEN size), `UART_JETSON_BRIDGE.md` (frame length table).
- Move this doc to `Completed_Plans/` and update the README index.

## Risks

- **RAM retention is empirical, not guaranteed by datasheet contract** for all reset
  paths; the magic + inverted-byte check makes corruption fail safe (→ `ZONE_UNKNOWN`).
- **Bootloader RAM usage** between reset and app start could theoretically clobber the
  breadcrumb; same mitigation. If it proves flaky in practice, relocate the section to
  a reserved word just below the bootloader's double-tap address instead.
- **Version skew**: an un-updated edge counts 11-byte AWAKENs as oversized frames only
  if it also enforces exact lengths — it doesn't (min/max range check), but
  `decode_awaken` must be made length-adaptive *before* nodes are reflashed or awaken
  records silently lose fields. Rollout order above handles this.
- **Attribution granularity**: breadcrumb reports the last *marked* region. A hang in
  unmarked code reports `ZONE_LOOP_IDLE` — informative but not a diagnosis; extend the
  zone set as needed.
