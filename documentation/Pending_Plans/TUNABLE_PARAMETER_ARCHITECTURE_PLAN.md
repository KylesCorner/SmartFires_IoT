# Tunable Parameter Architecture Plan

## Purpose

Restructure how tuning parameters are declared and consumed across the node
firmware, base firmware, and Jetson edge receiver so that:

1. Every tunable has exactly **one** place where its default lives.
2. Related tunables are grouped by domain, not by which class happens to own
   them.
3. Changing a value is a one-line edit with no hunting across files.
4. Invalid/incompatible combinations are caught at compile time (firmware) or
   startup (edge receiver) instead of in the field.

This plan is about **code structure**, not about what the values should be.
It does not change runtime behavior — every default called out below is kept
identical unless explicitly marked otherwise. Choosing new operating values
(slot widths, ACK cadence, sample rates) is a separate exercise that happens
*after* this restructuring, using whichever single file this plan creates as
the editing surface.

This plan composes with, and does not duplicate, two existing pending plans:

- [`NETWORK_PARAMETER_CONSOLIDATION_PLAN.md`](NETWORK_PARAMETER_CONSOLIDATION_PLAN.md) —
  governance/ops process for *changing* network parameters (profiles, approval
  classes, rollback criteria). That plan assumes the values live in sensible
  places; this plan is what makes that assumption true, and extends the same
  treatment to sensor sample rates, duty cycling, and the Jetson side, which
  the network plan explicitly left out of scope.
- [`ACK_PACED_RETRANSMIT_PLAN.md`](ACK_PACED_RETRANSMIT_PLAN.md) — a specific
  behavior change that adds new `TdmaConfig` fields. New fields it introduces
  should land directly in the consolidated location this plan defines, not in
  a second scattered spot.

## Problem Statement

Tunables currently live in at least **six different shapes** across the
codebase, with no single rule for which shape applies where:

1. **`platformio.ini` build flags** (`-DNUM_SLOTS=4`, `-DSMARTFIRES_STATUS_INTERVAL_MS=...`) —
   consumed via `#ifndef`/`#define` guards duplicated in `main.cpp`.
2. **Struct defaults baked into factory-method positional args**, e.g.
   `TdmaConfig::tdmaCfg(...)` and `DutyCycleConfig::dutyCycleCfg(...)` —
   8–13 positional parameters each with inline defaults, so reading the
   *actual* default requires scanning a function signature, not a value
   table.
3. **Per-call-site overrides on top of those defaults**, e.g.
   `makeNodeTdmaCfg()` in `main.cpp` re-assigns 11 fields after constructing
   from `tdmaCfg()` — meaning the "default" in `TdmaConfig.h` is not actually
   what ships on a node.
4. **Private `constexpr` constants buried in class headers**, e.g.
   `SmartFiresBaseApp::kPeriodicTimeSyncMs`, `kHealthLogPeriodMs`, and
   `SmartFiresNodeApp::kAwakenIntervalMs` — invisible from any config struct,
   not overridable, not listed in any doc.
5. **Per-sensor `Config` structs**, each with its own independent
   `minSamplePeriodMs`/`wakeDelayMs` defaults (6 sensors, 6 separate
   factories) with no shared notion of "system sample rate."
6. **Python `argparse` defaults duplicated across subcommands** — `receive`
   and `web` in `main.py` declare nearly identical flag sets independently,
   so adding or changing a default means editing it 2–3 times and hoping they
   stay in sync.

### Concrete redundancy/drift already found

- `TdmaConfig.maxRetries` and `RadioHeadTdmaDriver::Config.retries` are two
  independent fields meant to represent the same link-layer retry count.
- `TdmaConfig.ackTimeoutMs` and `RadioHeadTdmaDriver::Config.timeoutMs` are
  two independent fields for the same thing; `main.cpp` manually threads one
  into the other (`makeNodeRadioCfg`), but nothing enforces this everywhere
  such a config is built (e.g. dummy node).
- **Base station TDMA geometry is a second, disconnected copy.**
  `SmartFiresBaseApp::Config::baseCfg()` hardcodes its own
  `tdmaNumSlots=4`, `tdmaSlotWidthMs=900`, `tdmaGuardMs=20` defaults, used for
  the base's own ACK-summary slot-window math
  (`SmartFiresBaseApp.cpp:591-935`). The base build does not even receive
  `-DNUM_SLOTS`. If a node deployment changes `NUM_SLOTS` (the repo's own
  CLAUDE.md instructs reflashing *all node Feathers* when this happens), the
  base station silently keeps stale geometry with no compiler warning and no
  doc cross-reference telling anyone to update it.
- **Two independent "TIME_SYNC interval" knobs** exist for the same
  conceptual cadence: the base firmware's hardcoded `kPeriodicTimeSyncMs`
  (50,000 ms fallback broadcast) and the Jetson's `--sync-interval` CLI flag
  (600,000 ms default, sent to the base over UART). They are off by 12x and
  nothing documents that both exist.
- Sensor `minSamplePeriodMs` floors (100/10/1000/10/varies/1000 ms across
  SHT31, wind, SPS30, IMU, GPS, battery) are set independently of the actual
  cadence driver, `DutyCycleConfig.samplePeriodMs` (currently 500 ms via
  `dutyCycleCfgContinuous()`), with no validation that any floor is
  compatible with the cadence that's supposed to drive it.

## Scope

In scope:

1. Node firmware config (TDMA/radio/reliability, sensors, duty cycle,
   battery).
2. Base firmware config (UART bridge, ACK-summary slot gating, periodic
   sync).
3. Jetson edge-receiver runtime config (CLI args across `receive`/`web`/
   `visualize`/`cli`, anemometer poller).
4. A single human-readable reference doc that supersedes the scattered
   mentions of defaults in `SOFTWARE_DESIGN.md`, `TDMA_PROTOCOL.md`, and
   `DUTY_CYCLING.md`.

Out of scope (unchanged by this plan):

1. Picking new operating values — covered by
   `NETWORK_PARAMETER_CONSOLIDATION_PLAN.md`'s profile/approval process once
   this restructuring lands.
2. Wire protocol / packet format changes.
3. Adding persistent runtime config storage (e.g. SD card / flash-backed
   settings) to the MCU firmware — explicitly rejected below.

## Guiding Principles (Industry Practice Applied Here)

1. **Single source of truth.** Every parameter has exactly one file where its
   default is declared. Everything else either reads that value or
   explicitly derives from it — it never re-declares it.
2. **Compile-time config for the MCU, runtime config for the edge.** The
   Feather M0 boards have no persistent writable storage and reflashing is
   already the deployment mechanism, so firmware tunables stay
   `constexpr`/struct defaults resolved at build time — introducing a
   runtime config file (JSON/SD card) on the MCU would add flash/RAM cost and
   a new failure mode for no real benefit. The Jetson, by contrast, runs from
   a normal filesystem and is reconfigured per-deployment, so it should move
   from copy-pasted CLI defaults to a layered config (defaults → optional
   config file → CLI override).
3. **Config is data, not behavior.** Config structs only hold values; they
   never compute derived behavior beyond simple, obviously-related
   derivations (e.g. "radio timeout = TDMA ack timeout"). This is already
   mostly true in this codebase (constructor injection of config structs into
   classes) — the gap is that *defaults* leak into multiple places instead of
   the structs being populated from one place.
4. **Group by domain, not by class ownership.** A developer tuning "how often
   do we sample" should open one file, not six. Domains: **Network**
   (TDMA/radio/reliability), **Sensing** (per-sensor cadence + duty cycle),
   **Power** (battery), **Base bridge** (UART/ACK-summary/sync), **Edge
   runtime** (Jetson CLI/poller).
5. **Fail loud, fail early.** Cross-field invariants (e.g. slot width must
   exceed worst-case TX+ACK time, sensor floor must not exceed the duty-cycle
   period driving it, `NUM_SLOTS` must match between node and base builds)
   become `static_assert`s or boot-time validation logs — not tribal
   knowledge in a markdown file.
6. **No positional-default-argument factories.** Replace
   `make(a=1, b=2, c=3, ...)` 10-parameter factories with a plain struct
   literal using designated-style field assignment. This is strictly easier
   to read, diff, and partially override, and avoids silent bugs from
   reordering parameters.
7. **DRY the Python CLI surface.** One function builds the shared argument
   group; subcommands call it instead of repeating `add_argument` blocks.

## Target Layout — Firmware

```
platformio/include/config/
├── NetworkConfig.h     # TDMA geometry + radio + link-ACK + app reliability
│                         (replaces scattered TdmaConfig defaults,
│                          RadioHeadTdmaDriver::Config defaults, and the
│                          main.cpp override block)
├── SensingConfig.h      # Duty cycle cadence + per-sensor sample/wake floors,
│                         declared together so the relationship between the
│                         master cadence and each sensor's floor is visible
│                         in one place
├── PowerConfig.h        # Battery monitor thresholds/sampling
└── BaseConfig.h          # Base station bridge: UART baud, ACK-summary
                            cadence, periodic TIME_SYNC interval, health-log
                            interval — and the SAME NetworkConfig::Geometry
                            type used by the node, so NUM_SLOTS/slotWidthMs/
                            guardMs cannot drift between node and base
```

Rules for this directory:

- These headers contain **only** data (structs + `constexpr`/default
  values), no logic.
- `TdmaConfig.h`, `DutyCycleConfig` (in `DutyCycleController.h`),
  `BatteryMonitor::Config`, `RadioHeadTdmaDriver::Config`,
  `SmartFiresBaseApp::Config` keep their existing struct *shapes* (consuming
  classes are unchanged) — only the **defaults** move into `config/`, and
  each per-class factory becomes a thin wrapper that returns the canonical
  struct from `config/`, rather than declaring its own defaults inline. This
  keeps the dependency-injection pattern already used throughout the
  firmware intact.
- `main.cpp` for both node and base roles stops hand-assembling overrides
  (`makeNodeTdmaCfg()`'s 11-line override block disappears) and instead pulls
  a single named profile struct from `NetworkConfig.h`.
- The node/dummy-node/debug build-flag `#ifndef` guards currently duplicated
  in `main.cpp` move into `NetworkConfig.h`/`SensingConfig.h` as the one place
  that reads `-D` build flags and turns them into struct fields.
- Add `static_assert`s in `NetworkConfig.h` for the invariants already
  documented in prose today, e.g.:
  - `slotWidthMs > 2 * (maxBundleTxBudgetMs + ackTimeoutMs) + 2 * guardMs`
  - `retryWaitMinMs <= retryWaitMaxMs`
  - `queueDepth <= TdmaTxQueue::MaxDepth` and
    `reliabilityWindowDepth <= TdmaRadioService::kMaxReliabilityWindow`
- Add a boot-time validation pass (already have a precedent: `main.cpp`
  logs every TDMA field at startup) that warns if any sensor's
  `minSamplePeriodMs` exceeds `SensingConfig::samplePeriodMs` — today this can
  silently mean a sensor never produces fresh data within a sensing cycle.

## Target Layout — Base Firmware Specifics

- `BaseConfig.h` imports the **same** TDMA geometry type/values that
  `NetworkConfig.h` defines for the node, instead of
  `SmartFiresBaseApp::Config::baseCfg()` hardcoding its own copy. This closes
  the drift gap described above.
- `kPeriodicTimeSyncMs`, `kHealthLogPeriodMs`, `kMaxAssignedNodes`,
  `kMaxAckTrackedNodes` move from private `constexpr` in
  `SmartFiresBaseApp.h` into `BaseConfig.h` fields with documented defaults,
  so they are visible and overridable without touching implementation code.
- Document, in the same file, the relationship between
  `BaseConfig::periodicTimeSyncMs` (base-firmware fallback broadcast) and the
  Jetson's `EdgeConfig.sync_interval_s` (primary cadence sent over UART) —
  these are legitimately two different mechanisms (local fallback vs.
  Jetson-driven), but today nothing states that, so the 50 s vs. 600 s gap
  reads like a bug instead of a documented failsafe interval.

## Target Layout — Jetson Edge Receiver

```
edge/edge-receiver/src/smartfires_edge/
└── config.py
    ├── IngestConfig      # port, baud, data_dir, nodes, metrics_interval_s,
    │                       sync_interval_s, fsync_every_row, raw_log
    ├── AnemometerConfig   # port, baud, address, interval_s
    └── EdgeConfig         # composes the above + web/cli-specific fields
                             (host, http_port, session_file)
```

- Add `add_common_ingest_args(parser)` and `add_anemometer_args(parser)`
  helper functions in `config.py`; `receive` and `web` subcommands in
  `main.py` both call them instead of repeating the ~12-argument block
  independently. `visualize`/`cli` call the relevant subset.
- Add `EdgeConfig.from_args(args)` so each subcommand handler in `main.py`
  passes one object instead of spelling out 10+ keyword arguments at every
  call site (`run_receive(port=..., baud=..., ...)` becomes
  `run_receive(EdgeConfig.from_args(args))`).
- Layer config resolution as: **built-in default in `config.py`** → optional
  `--config path/to.toml` file (new, optional — falls back silently if
  absent) → explicit CLI flag (highest precedence, since operators routinely
  override one flag per run, e.g. `--sync-interval`). This is the standard
  CLI-tool layering (cf. how most ops tools resolve config) and means a fixed
  per-deployment config can be checked into ops material without retyping
  flags, while ad hoc overrides still work.
- Hardcoded values currently inline in `cli.py` (`5.0` s ACK-warning timeout,
  `60` s calibration duration) move into `IngestConfig`/`EdgeConfig` fields
  with the same defaults.

## Documentation Consolidation

- Create `documentation/Current_Architecture/TUNABLE_PARAMETERS.md`: one
  table per domain (Network, Sensing, Power, Base Bridge, Edge Runtime),
  columns `Parameter | File | Default | Units | Notes/Invariants`. This
  becomes the canonical reference that `SOFTWARE_DESIGN.md`,
  `TDMA_PROTOCOL.md`, and `DUTY_CYCLING.md` link to instead of restating
  numbers inline (those docs keep prose explanations of *why*, this doc is
  the single *what the value is and where it lives* reference).
- `NETWORK_PARAMETER_CONSOLIDATION_PLAN.md`'s parameter catalog (tables A–F)
  becomes a profile/governance overlay on top of this reference rather than
  a second inventory — once this plan lands, update that file's "Current
  Source" column to point at the new `config/` headers instead of
  `TdmaConfig.h`/`main.cpp` directly.

## Migration Phases

### Phase 0 — Freeze (no code changes)
Confirm the inventory above against current `main` (already done in this
session). Snapshot current build outputs (`pio run -e <env> --target
size`... not required, but note current behavior is the regression
baseline).

### Phase 1 — Introduce `config/` headers (additive only)
Create `NetworkConfig.h`, `SensingConfig.h`, `PowerConfig.h`, `BaseConfig.h`
with values copied verbatim from current defaults/overrides. Nothing
includes them yet. Add a native unit test (`test/test_config/`) that
`static_assert`s/asserts these new values equal the old call-site values, as
a tripwire against accidental drift during the copy.

### Phase 2 — Migrate node call sites
Point `main.cpp`'s node role, `SmartFiresNodeApp`, `PacketHandler`, and each
sensor's construction at the new headers. Delete the now-redundant
positional-default factories in favor of thin wrappers, and delete
`makeNodeTdmaCfg()`'s manual override block. Run native tests + a bench
flash to confirm identical boot log output (the existing `LOG_INFO("tdma",
...)` lines at boot are a convenient before/after diff).

### Phase 3 — Migrate base call sites
Point `SmartFiresBaseApp::Config::baseCfg()` at the shared TDMA geometry type
from `NetworkConfig.h`. Move the four private `constexpr`s into `BaseConfig.h`.

### Phase 4 — Add validation
Add the `static_assert`s and the sensor-floor-vs-duty-cycle boot warning
described above. Confirm they currently pass (they should, since Phase 1–3
didn't change values) — this is what proves the invariants were real and
just undocumented.

### Phase 5 — Jetson config consolidation
Add `config.py`, refactor `main.py` subcommands to use it, add the shared
argparse helpers, add optional config-file loading. Keep CLI behavior
backward compatible — same flags, same defaults, same precedence for anyone
not using a config file.

### Phase 6 — Documentation
Write `TUNABLE_PARAMETERS.md`, link it from `documentation/README.md` and
from `NETWORK_PARAMETER_CONSOLIDATION_PLAN.md`, and trim the now-duplicated
numeric tables out of `TDMA_PROTOCOL.md`/`DUTY_CYCLING.md` in favor of a
link.

### Phase 7 — Cleanup
Remove dead `TelemetryBuilder.h`/`RadioService.h` (confirmed unused — not
included from anywhere) while in the area, since leaving unused config-like
headers around undermines "one source of truth" for anyone grepping for a
parameter.

## Risks / Things to Watch

1. **Phase 2/3 are the only phases that touch shipped behavior indirectly**
   (by construction they shouldn't change values, but a copy-paste slip is
   possible) — gate them on the Phase 1 tripwire test passing and a real
   flash + boot-log diff, not just a clean compile.
2. Combining base and node TDMA geometry into one shared type means the base
   build must now pull in a header currently scoped to node-only
   (`NetworkConfig.h`) — keep that header free of node-only includes (radio
   driver headers, sensor headers) so it stays includable from both roles
   without pulling unrelated dependencies into the base build.
3. Jetson config-file loading is additive and optional — do not make any
   subcommand require a config file, to avoid breaking existing deployment
   scripts that only pass flags.

## Acceptance Criteria

1. Every parameter in the inventory above resolves to exactly one default
   declaration site, in a file named for its domain under
   `platformio/include/config/` (firmware) or `smartfires_edge/config.py`
   (Jetson).
2. No factory method has more than ~4 positional defaulted parameters; wider
   structs are built via field assignment on a named struct instance.
3. `NUM_SLOTS`/`slotWidthMs`/`guardMs` cannot drift between node and base
   builds because both read the same declaration.
4. `main.py` subcommands share argument-group construction instead of
   repeating `add_argument` blocks.
5. `TUNABLE_PARAMETERS.md` exists, is linked from `documentation/README.md`,
   and is the single page a new contributor is pointed to when asked "where
   do I change the sample rate / slot width / sync interval."
6. All native unit tests pass after each phase; boot-log output (node and
   base) is byte-for-byte equivalent before/after Phases 2–3 for unchanged
   values.
