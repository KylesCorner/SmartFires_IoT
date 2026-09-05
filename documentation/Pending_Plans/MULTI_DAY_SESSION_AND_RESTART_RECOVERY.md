---
name: multi-day-session-and-restart-recovery
description: Plan for dated dashboard timestamps and complete edge restart with a fresh session on Jetson boot or New Session.
category: plan-pending
status: draft
related_docs:
  - jetson-bridge
  - jetson-cheatsheet
  - smartfires-manager
  - reset-system
  - bundle-timestamp-fix
---

# Multi-day sessions and Jetson restart recovery

Requested 2026-09-04; planning review completed 2026-09-05. Implementation and hardware verification remain pending.

## Required behavior

1. **Primary:** every dashboard wall-clock timestamp includes its calendar date so readings on different days are distinguishable.
2. Every fresh Jetson boot starts the edge application from its entrypoint and creates a new recording session automatically.
3. **New Session** fully restarts the edge application, including ingest, timestamp decoding, optional sensor/sniffer workers, queues, and dashboard state. It must work with a silent or disconnected base.

Preserve prior session files, configured node identities, base coordinates, configuration, and map tile cache. A full edge reset is not a factory reset or a request to hard-reset every sensor node. Ordinary base USB reconnection within a running process should retain the current recording session.

## Findings from the current source

Paths below are relative to `edge/edge-receiver/src/smartfires_edge/` unless stated otherwise.

| Area | Observed gap |
|---|---|
| `web/static/js/nav.js`, `api.js` | Shared clock and last-seen formatter show time only. |
| `web/static/js/main_page.js` | Chart ticks, paused ranges, session timeline endpoints, activity tooltips, and AWAKEN tooltips omit dates. |
| `web/static/js/sniffer_page.js`, `map_history_page.js` | Sniffer paused ranges, plot ticks, and last-seen values need dates. Reboot history and sniffer wall-time detail already carry dates but need consistent formatting. |
| `web/static/js/live_log_page.js` | Both initial/filter rendering and appended rows strip the date from server ISO timestamps. Export already retains it. |
| `web/static/js/debug_page.js`, `live_state.py` | Debug rendering generates the current browser time each time a row is rendered, changing historical timestamps on filtering/export. The server stamps the disk log separately but does not attach that wall time to the streamed record. |
| `ingest_service.py`, `uart_receiver.py` | Startup base soft reset and initial TIME_SYNC run only after the serial generator yields its first decoded event. A silent stream can delay initialization indefinitely. |
| `web/app.py`, `ingest_service.py` | New Session only sets an event checked within packet processing. It can stall without traffic and resets only selected state. The existing `FrameReceiver` retains the previous session start, risking incorrectly dated samples afterward. |
| `web_service.py` | Ingest and optional sniffer run as daemon threads. Ingest failure can leave the HTTP server alive with no working receiver. |
| Deployment | No systemd unit is tracked in the repository. `edge/smartfires-manager.sh` assumes one is installed; `edge/start_receiver.sh` invokes `receive`, not `web`. The deployed unit and boot journal must be inspected before attributing the reported power-cycle failure to a specific cause. |

`run_receive()` already creates a new session ID on entry. The work is to ensure the service actually reaches full initialization, and to make New Session follow that same lifecycle. The startup ordering is a source-level finding; it has not been proven to be the sole cause on the deployed Jetson.

## Workstream 1 — dates throughout the dashboard

1. Introduce a shared formatter with explicit epoch-seconds, epoch-milliseconds, and ISO input handling. Use browser-local date/time consistently, display the timezone, and include a numeric UTC offset where repeated daylight-saving times could be ambiguous. Use an unambiguous full date such as `2026-09-05`; retain seconds and log milliseconds where useful.
2. Apply it to every surface listed above, including chart tooltip titles and any canvas labels discovered during implementation. Chart ticks may use two lines for date/time and fewer ticks on narrow screens; every displayed absolute time must still include its date.
3. Stamp debug records once on receipt at the server. Reuse that timestamp for stream, disk log, filtering, rerendering, and export. Preserve firmware `t` as a separately labeled uptime counter.
4. Audit UTC serialization, including ingest paths that currently emit timestamps without a timezone suffix. Preserve the meaning of existing stored UTC values when reading older files; do not reinterpret them as browser-local time. Keep exported machine timestamps in explicit UTC.
5. Keep elapsed durations, window sizes, slot offsets, and firmware counters labeled as durations/counters. Do not invent calendar dates for those values.

This work does not add historical-session browsing or extend bounded live-log/sniffer retention. Verify the existing disk-backed telemetry history across several days as part of the acceptance tests.

## Workstream 2 — one complete startup/restart lifecycle

### Establish the deployed service contract

Capture `systemctl cat smartfires-edge.service`, its effective command/user/working directory/restart settings, enablement, and current/previous boot journals on the Jetson. Confirm the installed package revision, configured data mount, USB symlinks, and optional devices.

Plan a repository-owned service template and installation instructions that launch the venv's `smartfires-edge web` entrypoint with the intended configuration. Ensure boot enablement, the configured data mount being available before recording, stable USB paths, and retrying delayed USB enumeration. Missing optional devices must not prevent core ingest from starting. Record startup failures in the journal and avoid silently writing to the root filesystem when the intended data mount is absent.

### Restart the whole application

Use one process lifecycle for fresh boot and New Session. Preferred deployed design: acknowledge the HTTP request, request graceful application shutdown, then let the service supervisor launch the entrypoint again. Configure restart policy to cover the intentional exit as well as failures, with bounded backoff; an explicit operator service stop must remain stopped. Resolve and document equivalent restart behavior for manually launched `web` before implementation is considered complete.

- Make shutdown independent of receiving serial packets. Stop/join workers, interrupt reconnect waits, close serial handles, stop TIME_SYNC, and flush/close the current session's writers and trackers with a bounded shutdown deadline.
- Recreate all process-local objects on restart: parser/session epoch, command and sync sequences, trackers, caches, queues, sniffer anchor, and enabled sensor workers. Do not replay commands queued in the previous session.
- Generate a fresh session ID and collision-safe session directory even after rapid restarts or a system-clock correction. Never append a fresh session to an existing directory just because its second-resolution name matches.
- Supervise required ingest-worker exit so the process cannot remain apparently healthy after ingest dies. Report optional-worker failures and handle their recovery without an endless full-process restart loop.
- Do not require the web process to run privileged `systemctl` commands. Return the restart response before shutting down; coalesce repeated button presses into a single restart.
- Show “Restarting…” in the browser and disable duplicate submissions. Reconnect after the outage and confirm a changed session ID before reporting success. Clear old charts, history caches, live/debug logs, and sniffer state on every open page, including pages that missed the restart response. Provide a visible timeout/error if recovery fails.

### Bootstrap serial before waiting for traffic

Separate serial opening/bootstrap from packet iteration. After opening the base port, execute the startup base soft reset and initial TIME_SYNC without requiring an incoming frame, then begin normal receive/sync operation. Handle port re-enumeration during initialization and make shutdown observable while the port is silent or absent. Mark the link connected only after a successful open and distinguish connection from readiness.

Keep session ID/start consistent across storage, parser, TIME_SYNC, and UI. Define handling for buffered samples from the previous session during the transition so they cannot be reconstructed against the new epoch and silently contaminate the new recording. Validate resynchronization with sleeping nodes as well as active ones.

The historical `Completed_Plans/RESET_SYSTEM.md` records that New Session hardware resets were reverted after field problems. This request changes the edge restart contract. Reusing the existing startup **base soft reset** must be tested explicitly; do not restore the former all-node hard-reset sequence. Dedicated node-reset controls remain separate.

## Acceptance and completion

| Test | Required result |
|---|---|
| At least 72 hours of synthetic telemetry crossing midnight and a month/year boundary | Every absolute timestamp on every page, chart, tooltip, and log includes the correct date; history stays ordered and accessible. |
| Two browser timezones and a daylight-saving repeated hour | Same underlying instants, correct local dates, explicit timezone/offset; UTC exports retain their meaning. |
| Debug filtering, reconnect, and export | A record's wall timestamp never changes when rerendered. |
| Cold boot with base immediately available, delayed, and initially silent | Service starts automatically, creates a new session, and sends initialization without waiting for traffic. |
| Jetson power removal/restoration and ordinary reboot | New process/session, preserved previous files, recovered ingest without manual intervention; capture journal evidence. |
| New Session during traffic, silence, and reconnect backoff | One full restart and fresh session; no leftover workers, serial owners, queued commands, old UI data, or old parser epoch. |
| Rapid repeated requests and multiple browser tabs | One accepted restart at a time; all tabs converge on the new session; no directory reuse. |
| Base-only USB reconnect; optional sensor/sniffer absent | Core ingest recovers; ordinary USB reconnect retains its session; missing optional hardware does not cause a restart storm. |
| Required worker exits unexpectedly; explicit operator service stop | Failure triggers supervised recovery; deliberate service stop remains stopped. |
| Data mount unavailable; clock changes after offline boot | Failure is visible and recoverable; no accidental fallback storage, overwritten sessions, or silently misdated data. |

Implement in order: timestamp formatting and regression coverage; process lifecycle and serial bootstrap; service packaging and deployment guidance; browser restart recovery; host tests and Jetson acceptance run. No firmware change is assumed. Hardware tests and deployment are future execution steps, not performed by this planning pass.

At implementation time, also add `plan-pending` / `draft` support, directory/category validation, and the existing no-`source_refs` rule to `documentation/check_doc_freshness.py`. The documented metadata is restored now; the validator update is deferred to honor the documentation-only request.

After validation, update the current Jetson bridge and operator references to describe the shipped behavior, record test evidence and any residual limitations, and move this plan to `Completed_Plans/`. Do not mark complete from a service “active” status alone.
