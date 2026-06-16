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

This plan supersedes two former standalone pending plans, which have been
folded in as appendices rather than left as separate, drifting files:

- **Appendix A — Network Parameter Catalog & Governance** (formerly
  `NETWORK_PARAMETER_CONSOLIDATION_PLAN.md`): the governance/ops process for
  *changing* network parameters (profiles, approval classes, rollback
  criteria). That plan assumed the values already lived in sensible places;
  the main body of this plan is what makes that assumption true, and extends
  the same treatment to sensor sample rates, duty cycling, and the Jetson
  side, which the original network plan explicitly left out of scope.
- **Appendix B — ACK-Paced Retransmit Feature Plan** (formerly
  `ACK_PACED_RETRANSMIT_PLAN.md`): a specific reliability behavior change
  that adds `TdmaConfig` fields for ACK-paced retry gating. Those fields
  belong in the consolidated `NetworkConfig.h` this plan defines, not in a
  second scattered spot — see the status note at the top of Appendix B for
  how far that work has actually progressed in code already.

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

1. Picking new operating values — covered by Appendix A's profile/approval
   process once this restructuring lands.
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
- Appendix A's parameter catalog (tables A–F) is a profile/governance overlay
  on top of this reference rather than a second inventory — once Phases 1–3
  land, update Appendix A's "Current Source" column to point at the new
  `config/` headers instead of `TdmaConfig.h`/`main.cpp` directly.

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
from Appendix A below, and trim the now-duplicated numeric tables out of
`TDMA_PROTOCOL.md`/`DUTY_CYCLING.md` in favor of a link.

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

---

## Appendix A: Network Parameter Catalog & Governance

*Merged from the former `NETWORK_PARAMETER_CONSOLIDATION_PLAN.md`. This is
the operational/governance layer for the **Network** domain specifically —
it assumes the structural work in the main body of this plan (a single
`NetworkConfig.h`) has landed, and "Current Source" columns below should be
updated to point there once Phases 1–3 are done.*

### Purpose

Provide a single source of truth for *operating* SmartFires network tuning
parameters (as opposed to where they're declared in code, which is the main
body's concern) across:

- Node firmware (TDMA, queueing, reliability)
- Base firmware bridge behavior (health and forwarding context)
- Edge receiver runtime (ACK and sync cadence)

The objective is to make network tuning deterministic, auditable, and safe
to change without hidden coupling between components.

### Problem Statement

Network tuning inputs are distributed across compile-time defines
(`platformio.ini`), firmware defaults (`TdmaConfig`, node setup logic), edge
CLI runtime flags (`smartfires-edge receive`), and architecture docs spread
across multiple files. This fragmentation increases risk of:

1. conflicting assumptions (for example ACK cadence vs retry windows)
2. accidental regressions during field tuning
3. difficult root-cause analysis when reliability changes

### Scope

In scope:

1. Inventory all network-related tunables in one place.
2. Define ownership and change process per parameter group.
3. Define baseline profiles (debug, lab, production).
4. Define compatibility rules between parameters.
5. Define rollout/rollback and validation gates.

Out of scope:

1. protocol redesign of packet formats.
2. hardware changes (antenna, radio module, power chain).
3. replacing AppLayerAckSummary architecture.

### Source Locations (Current)

Primary code/config sources:

1. `platformio/platformio.ini`
2. `platformio/include/radio/TdmaConfig.h` (target: `platformio/include/config/NetworkConfig.h` once main-body Phase 1–3 lands)
3. `platformio/include/radio/TdmaTxQueue.h`
4. `platformio/src/main.cpp`
5. `edge/edge-receiver/src/smartfires_edge/main.py` (target: `smartfires_edge/config.py` once main-body Phase 5 lands)

Supporting architecture docs:

1. `documentation/Current_Architecture/TDMA_PROTOCOL.md`
2. `documentation/Current_Architecture/PACKET_RELIABILITY.md`

### Consolidated Parameter Catalog

#### A) Build-Time and Environment Parameters

| Parameter | Layer | Current Default | Current Source | Owner | Change Frequency | Notes |
|---|---|---:|---|---|---|---|
| `NUM_SLOTS` | Node firmware | `4` | `platformio.ini` | Firmware | Low | Must match all nodes in deployment |
| `SMARTFIRES_TDMA_RELIABILITY_MODE` | Node firmware | `1` (`APP_ACK_SUMMARY`) | `platformio.ini` | Firmware | Low | Keep `1` in this plan |
| `SMARTFIRES_STATUS_INTERVAL_MS` | Node app packet generation | `1000` debug / `2500` node env / fallback 15 min | `platformio.ini`, `main.cpp` | Firmware + Ops | Medium | Directly influences offered load |
| `monitor_speed` | Serial monitor | `115200` | `platformio.ini` | Ops | Low | Debug transport only |

#### B) TDMA Timing Parameters (Node)

| Parameter | Layer | Current Default | Current Source | Owner | Safe Initial Range |
|---|---|---:|---|---|---|
| `slotWidthMs` | TDMA | `900` | `TdmaConfig` | Firmware | 800 to 1200 |
| `guardMs` | TDMA | `20` | `TdmaConfig` | Firmware | 10 to 40 |
| `syncStaleMs` | TDMA | `1320000` (22 min) | `TdmaConfig` | Firmware | 600000 to 1800000 |

#### C) Queue and Buffering Parameters (Node)

| Parameter | Layer | Current Default | Current Source | Owner | Hard Cap / Range |
|---|---|---:|---|---|---|
| `queueDepth` | TX queue | `4` default / `8` node override | `TdmaConfig` and node setup | Firmware | Runtime <= 8 (current hard cap) |
| `TdmaTxQueue::MaxDepth` | TX queue capacity cap | `8` | `TdmaTxQueue.h` | Firmware | Compile-time cap |
| `reliabilityWindowDepth` | Pending reliability window | `4` default / `8` node override | `TdmaConfig` | Firmware | Runtime <= 8 |
| `kMaxReliabilityWindow` | Pending window cap | `8` | `TdmaRadioService.h` | Firmware | Compile-time cap |
| `MaxPayloadLen` | Payload buffer capacity | `220` | `TdmaConfig` | Firmware | Must stay >= max packet size |

#### D) Link-ACK Path Parameters (Node)

| Parameter | Layer | Current Default | Current Source | Owner | Safe Initial Range |
|---|---|---:|---|---|---|
| `enableLinkAck` | Link-layer behavior | computed by mode | `main.cpp` | Firmware | mode dependent |
| `maxRetries` | Link ACK retries | `3` (node setup override) | `main.cpp` | Firmware | 0 to 5 |
| `ackTimeoutMs` | Link ACK timeout | `250` (node setup override) | `main.cpp` | Firmware | 80 to 400 |

#### E) App Reliability Parameters (Node)

| Parameter | Layer | Current Default | Current Source | Owner | Safe Initial Range |
|---|---|---:|---|---|---|
| `enableAppReliability` | App reliability | `true` | `TdmaConfig` | Firmware | true for this plan |
| `reliabilityMaxAttempts` | Pending retry limit | `3` | `TdmaConfig` | Firmware | 2 to 5 |
| `reliabilityMaxAgeMs` | Pending age limit | `15000` default / `30000` node override | `TdmaConfig` | Firmware | 10000 to 45000 |
| `reliabilityMinRetryGapMs` | Minimum retry spacing | `2000` | `TdmaConfig` | Firmware | 1500 to 8000 |
| `reliabilityFreshTrafficHoldoffMs` | Holdoff after fresh send | `2000` | `TdmaConfig` | Firmware | 1000 to 8000 |
| `expectedAckIntervalMs` | ACK-paced retry gate | `4000` | `TdmaConfig` | Firmware | see Appendix B |
| `retryWaitMultiplierPermille` | ACK-paced retry gate | `2000` (2.0x) | `TdmaConfig` | Firmware | see Appendix B |
| `retryWaitMinMs` / `retryWaitMaxMs` | ACK-paced retry gate | `4500` / `10000` | `TdmaConfig` | Firmware | see Appendix B |
| `requireAckSummaryBeforeFirstRetry` | ACK-paced retry gate | `false` | `TdmaConfig` | Firmware | see Appendix B |

#### F) Edge Runtime Parameters

| Parameter | Layer | Current Default | Current Source | Owner | Safe Initial Range |
|---|---|---:|---|---|---|
| `--sync-interval` | TIME_SYNC cadence (Jetson → base over UART) | `600` s | edge CLI parser | Edge/Ops | 120 to 900 |
| `--metrics-interval` | Metrics persistence | `10` s | edge CLI parser | Edge/Ops | 5 to 30 |
| `--nodes` | tracked node IDs | `[1, 2]` | edge CLI parser | Edge/Ops | deployment dependent |
| `--raw-log` | frame logging toggle | off by default | edge CLI parser | Ops | debug only |
| `kPeriodicTimeSyncMs` | Base firmware fallback TIME_SYNC broadcast | `50000` ms | `SmartFiresBaseApp.h` (private `constexpr`) | Firmware | distinct from `--sync-interval`; see main-body problem statement |

### Compatibility Rules (Must Hold)

1. `NUM_SLOTS` must be identical across all deployed nodes, and the base
   station's `tdmaNumSlots`/`tdmaSlotWidthMs`/`tdmaGuardMs` must match the
   node value (see main-body finding on base/node geometry drift).
2. If `SMARTFIRES_TDMA_RELIABILITY_MODE=1`, the ACK-paced retry gate fields
   (Appendix B) must be tuned together — `expectedAckIntervalMs` should
   reflect the base's actual ACK-summary flush cadence
   (`ackSummaryMinIntervalMs` plus batching delay), not be set independently.
3. Retry pacing should not be shorter than practical ACK cadence horizon.
4. If `queueDepth`/`reliabilityWindowDepth` are increased, verify memory
   headroom before deployment, and never exceed the compile-time caps in
   `TdmaTxQueue.h`/`TdmaRadioService.h`.
5. `SMARTFIRES_STATUS_INTERVAL_MS` and `NUM_SLOTS` must be tuned together to
   avoid chronic queue pressure.

### Baseline Parameter Profiles

#### Profile A: Debug (high visibility)

- `NUM_SLOTS=4`
- `SMARTFIRES_TDMA_RELIABILITY_MODE=1`
- `SMARTFIRES_STATUS_INTERVAL_MS=1000`
- `queueDepth=8` (current node default)
- edge `--sync-interval=600`
- verbose node/base logging enabled

#### Profile B: Lab Stress

- `NUM_SLOTS=4` or 6 (test-specific)
- `SMARTFIRES_TDMA_RELIABILITY_MODE=1`
- `SMARTFIRES_STATUS_INTERVAL_MS=1000 to 3000`
- `queueDepth=8`, `reliabilityWindowDepth=8`
- controlled RF attenuation/interference

#### Profile C: Production Candidate

- `NUM_SLOTS=4` (unless scaling decision changes)
- `SMARTFIRES_TDMA_RELIABILITY_MODE=1`
- `SMARTFIRES_STATUS_INTERVAL_MS=10000` (or approved value)
- queue/window values from validated test matrix
- edge `--sync-interval` fixed and documented in deployment playbook

### Governance and Ownership Model

#### Change Classes

Class 1 (Low risk):

- edge runtime only (`--sync-interval`, `--metrics-interval`, logging toggles)

Class 2 (Medium risk):

- firmware runtime defaults (`TdmaConfig` values, including Appendix B's
  ACK-paced retry fields)
- build flags affecting load (`SMARTFIRES_STATUS_INTERVAL_MS`)

Class 3 (High risk):

- compile-time caps (`MaxDepth`, `kMaxReliabilityWindow`)
- slot geometry changes (`slotWidthMs`, `guardMs`, `NUM_SLOTS`) — these now
  affect both node and base once the shared geometry type from the main body
  lands

#### Approval Path

1. Class 1: single maintainer approval + validation run.
2. Class 2: firmware + edge owner approval + A/B metrics.
3. Class 3: formal review with rollback plan and staged rollout.

### Required Change Record Template

For every parameter change, record:

1. parameter name
2. old value and new value
3. reason for change
4. expected impact
5. validation runs and metrics
6. rollback trigger and rollback value

### Validation and Observability Plan

#### Mandatory Metrics

1. Duplicate ratio at base/edge
2. Missing ratio
3. Retry amplification (`retx_sent / fresh_tx_sent`)
4. Queue pressure (`drop_oldest`, peak queue occupancy)
5. Pending pressure (`drop_pending`, peak pending occupancy)
6. ACK health (summary cadence consistency)

#### Required Logs

Node monitor (`radio`, `tdma`, `packet`) must include:

- `tx_sent`, `retx_candidate`, `retx_sent`
- `ack_summary_acked`, `ack_summary_needs_retx`
- `drop_oldest`, `drop_pending`

Base monitor (`base`) must include:

- `health_link`, `health_rx`, `rx_lora`, `tx_ack_summary`

Edge receiver logs must include:

- TIME_SYNC TX cadence indicators (`[EDGE][SYNC-TX#...]`)
- packet loss summary output

### Rollout Strategy

1. Establish baseline metrics with unchanged parameters.
2. Apply one parameter group at a time (no mixed large changes).
3. Validate against baseline with equivalent runtime duration.
4. Promote from debug -> lab stress -> production candidate.

### Rollback Criteria

Rollback if any condition persists:

1. duplicate ratio worsens versus baseline by >10 percent
2. missing ratio worsens beyond agreed tolerance
3. queue/pending drops rise above baseline for sustained windows
4. command/control responsiveness degrades

### Open Decisions

1. Whether to keep `SMARTFIRES_STATUS_INTERVAL_MS` split between debug and
   production envs or align test/prod closer.
2. Whether to raise compile-time queue/window hard caps beyond 8 after
   memory review.
3. Whether `kPeriodicTimeSyncMs` (base fallback) should become a documented,
   intentionally-longer-than-`--sync-interval` failsafe, or be removed in
   favor of relying solely on the Jetson-driven sync once that path is
   proven reliable.

---

## Appendix B: ACK-Paced Retransmit Feature Plan

*Merged from the former `ACK_PACED_RETRANSMIT_PLAN.md`.*

**Implementation status as of this merge:** the config fields and retry-gate
logic this plan called for already exist in code —
`TdmaConfig::expectedAckIntervalMs`/`retryWaitMultiplierPermille`/
`retryWaitMinMs`/`retryWaitMaxMs`/`requireAckSummaryBeforeFirstRetry` are
declared in `TdmaConfig.h`, and `TdmaRadioService::computeRetryWaitMs()` plus
the `ackGateOpened` pending-entry field and the `retx_blocked`/`retx_gate_open`
log lines are implemented in `TdmaRadioService.cpp`. The node-side buffer
expansion (`queueDepth=8`, `reliabilityWindowDepth=8`,
`reliabilityMaxAgeMs=30000`) is also live via `makeNodeTdmaCfg()` in
`main.cpp`. **Shipped defaults differ from this plan's original candidate
values** (e.g. `expectedAckIntervalMs` shipped as `4000` vs. the `2000`
candidate below; `requireAckSummaryBeforeFirstRetry` shipped as `false` vs.
the `true` candidate) — those were presumably adjusted during tuning, but
there is no record of why. Phases 0 (baseline capture) and 5 (validation
matrix execution) below have no recorded evidence of having been run — treat
those, plus the rollout staging in Phase 6, as the open remainder of this
plan rather than the config/logic work, which is done.

### Purpose

Reduce over-the-air (OTA) bandwidth consumed by duplicate telemetry
retransmissions while preserving the current reliability mode
(`APP_ACK_SUMMARY`).

This plan introduces ACK-paced retransmission gating on the node so retries
occur only after a realistic waiting window, instead of aggressive early
retries.

### Scope

In scope:

- Node-side reliability behavior in `TdmaRadioService`.
- TDMA queue and pending-window sizing for delayed retry strategy.
- Runtime defaults for retry timing and pending retention.
- Instrumentation and validation for bandwidth/reliability tradeoff.
- Documentation updates for operations and debugging.

Out of scope:

- Replacing `APP_ACK_SUMMARY` with `STRICT_LINK_ACK` as production default.
- Redesigning Jetson ACK summary protocol payload format.
- RF PHY changes (spreading factor, coding rate, TX power).

### Background

Current `APP_ACK_SUMMARY` behavior keeps fresh telemetry non-blocking and
performs app-layer reliability with a pending window.

Observed issue:

- Node retransmits some telemetry packets before a realistic ACK summary
  window elapses.
- Base receives duplicate packets (same telemetry packet sequence),
  increasing OTA load.

Root cause pattern:

- Retransmit eligibility was originally governed by retry gap/age only, not
  strict ACK pacing.
- If ACK summary cadence is slower than retry gate timing, retries happen
  early.

### Objectives

1. Keep `APP_ACK_SUMMARY` mode enabled.
2. Delay retransmission until a realistic ACK window has passed.
3. Maintain or improve end-to-end delivery reliability.
4. Reduce duplicate OTA packet transmissions and base duplicate reception.
5. Preserve fresh-telemetry priority over retransmits.

### Success Criteria

Primary:

1. Duplicate telemetry receptions at base decrease by at least 40 percent in
   comparable runs.
2. No statistically significant increase in missing packet ratio.
3. No sustained increase in queue overflow (`drop_oldest`) events.

Secondary:

1. Pending-window saturation remains below 80 percent under nominal load.
2. No regression in command/control responsiveness (`CMD_ACK` path).

### Design Overview (As Implemented)

#### Core Change: ACK-Paced Retry Gate

In `APP_ACK_SUMMARY`, a pending telemetry entry is eligible for retransmit
only when either condition is true:

1. **ACK-summary window condition** — at least one ACK summary cycle
   relevant to that entry has had a chance to arrive.
2. **Fallback timeout condition** — entry age exceeds a bounded fallback
   wait time to prevent deadlock if ACK summaries stall.

#### Eligibility Model

For each pending entry:

- `entry_age_ms = now - firstSentMs`
- `retry_wait_ms = clamp(expectedAckIntervalMs * retryWaitMultiplierPermille / 1000, retryWaitMinMs, retryWaitMaxMs)`

Retransmit allowed when `entry_age_ms >= retry_wait_ms` and the standard
gates also pass (`reliabilityMinRetryGapMs`, attempts/age caps, queue
priority rules). The stronger optional gate
(`requireAckSummaryBeforeFirstRetry`) requires at least one ACK summary
observed since entry creation before the *first* retry, unless the fallback
timeout is reached — implemented but currently shipped disabled (`false`).

### Remaining Open Work

#### Phase 0 (retro) — Baseline Capture

Not confirmed as having been run. Before changing any of the shipped
defaults above, capture node/base debug logs and a receiver packet-loss
summary as an A/B baseline.

#### Phase 5 — Validation Matrix

Run and record results for:

1. ACK interval 2.0 s, 1 node
2. ACK interval 4.0 s, 1 node
3. ACK interval 2.0 s, 2+ nodes
4. Loss-injected run (RF attenuation/interference), 2+ nodes

For each run capture duplicate packet counts, missing/loss counts,
queue/pending depth peaks, and retry counts/timing, then produce a pass/fail
report against the Success Criteria above.

#### Phase 6 — Rollout and Guardrails

1. Deploy to debug node profile first (already the case for current
   defaults).
2. Promote to production node profile only after the Phase 5 matrix passes.
3. Document the emergency rollback recipe (below) alongside a known-good
   configuration snapshot.

### Code Touchpoints (Reference)

1. `platformio/include/radio/TdmaConfig.h`
2. `platformio/src/radio/TdmaRadioService.cpp`
3. `platformio/include/radio/TdmaRadioService.h`
4. `platformio/src/main.cpp` (node config default values/logs)
5. `platformio/src/main_node_dummy.cpp` (parity for test profile)
6. `documentation/Current_Architecture/PACKET_RELIABILITY.md`
7. `documentation/User_Reference/DEBUG_FILTER.md` (monitoring grep patterns)

### Validation Metrics Definitions

1. Duplicate ratio — `duplicates / total_received_telemetry`
2. Missing ratio — `missing / (received + missing)`
3. Retry amplification — `retx_sent / fresh_tx_sent`
4. Queue pressure — peak `q=count/capacity`, count of `drop_oldest`
5. Pending pressure — peak `pendingCount/windowDepth`, count of `drop_pending`

### Test Command Set (Reference)

Node monitor:

```bash
SFDBG_SRC=boot,tdma,radio,packet SFDBG_MIN_LEVEL=D SFDBG_SHOW_RAW=0 pio device monitor -e feather_m0_lora_node_debug | tee /tmp/sf-node-debug.log
```

Base monitor:

```bash
SFDBG_SRC=base,app,radio,tdma,packet SFDBG_MIN_LEVEL=D SFDBG_SHOW_RAW=0 pio device monitor -e feather_m0_lora_base | tee /tmp/sf-base-debug.log
```

Edge receiver:

```bash
smartfires-edge receive --port /dev/ttyTHS1 --baud 115200 --data-dir /tmp/sf-ack-paced --sync-interval 600 --metrics-interval 5 --raw-log | tee /tmp/sf-receiver.log
```

Quick log extraction:

```bash
rg -n "retx_blocked|retx_candidate|retx_sent|ack_summary_acked|ack_summary_needs_retx|drop_pending|drop_oldest" /tmp/sf-node-debug.log
```

### Risks and Mitigations

1. **Risk:** delayed retries increase loss under sparse ACK summaries.
   **Mitigation:** fallback timeout gate and bounded max wait (implemented).
2. **Risk:** larger buffers increase SRAM usage.
   **Mitigation:** measure memory headroom in debug build; tune queue/window
   asymmetrically if needed.
3. **Risk:** over-conservative gating under heavy contention.
   **Mitigation:** tuning knobs for wait multiplier and min/max bounds
   (implemented, currently at shipped defaults above).
4. **Risk:** hidden regressions in command/control latency.
   **Mitigation:** include CMD/CMD_ACK checks in the Phase 5 validation
   matrix.

### Open Decisions

1. Should first retry require explicit ACK summary observed, or only elapsed
   wait window? (Currently shipped as elapsed-wait-only:
   `requireAckSummaryBeforeFirstRetry = false`.)
2. What is canonical expected ACK interval in production — the shipped
   `4000` ms, or a re-derived value tied to the base's actual
   `ackSummaryMinIntervalMs` batching cadence (see Appendix A, Compatibility
   Rule 2)?
3. Should queue depth and window depth both be 8 in production (current
   shipped state), or staged differently?

### Acceptance Checklist

1. ACK-paced gates implemented and configurable. ✅ done.
2. Node logs clearly show retry blocked/open reasons. ✅ done.
3. Duplicate ratio reduced at least 40 percent in lab matrix. ⬜ not yet
   validated/recorded.
4. Missing ratio non-regressing within agreed tolerance. ⬜ not yet
   validated/recorded.
5. No sustained queue overflow increase. ⬜ not yet validated/recorded.
6. Docs updated and rollback instructions included. ✅ this appendix.

### Rollback Plan

If regressions are observed:

1. Disable strict first-retry ACK dependency
   (`requireAckSummaryBeforeFirstRetry = false` — already the shipped
   default).
2. Reduce wait policy toward legacy values: lower multiplier, lower min
   wait.
3. If needed, revert to previous config constants while retaining
   instrumentation.
4. Keep `APP_ACK_SUMMARY` mode active unless directed otherwise.
