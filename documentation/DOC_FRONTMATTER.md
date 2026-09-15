# Documentation Frontmatter Schema

Every content `.md` file under `documentation/` carries a YAML frontmatter block at the top
of the file, before the H1. `DOC_FRONTMATTER.md` and `CODE_FRONTMATTER.md` are the two schema
definitions and are exempt. Read this page before adding or editing frontmatter, and update
it if the schema changes.

## Why

`Current_Architecture/` docs are supposed to match the code exactly. They drift anyway —
this session's full doc audit found multiple docs describing removed link-layer retry
defaults, stale packet sizes, and a feature ("Heading CLI") that was never built. Frontmatter
won't stop drift by itself, but `source_refs` + `last_verified` let `check_doc_freshness.py`
catch it mechanically: if a source file changed more recently than a doc's `last_verified`
date, the doc is flagged for re-review.

## Fields

```yaml
---
name: tdma-protocol                  # required — stable slug, see naming rule below
description: One-line summary.      # required
category: architecture               # required — see categories below
status: current                      # required — see statuses below
last_verified: 2026-06-23             # required for architecture/reference/index; omit for plans
source_refs:                         # required non-empty list for architecture; optional for
  - platformio/include/radio/TdmaConfig.h   # reference; omitted for plans/index
  - platformio/src/radio/TdmaRadioService.cpp
related_docs:                        # optional — list of other docs' `name` slugs
  - packet-reliability
superseded_by: null                  # optional — only on plan-completed docs; the architecture
---                                  # doc's `name` slug that now covers this area, if any
```

### `name` — slug naming rule

Lowercase the filename, strip `.md`, replace `_` with `-`. `JETSON_BRIDGE.md` →
`jetson-bridge`. `documentation/README.md` is the one exception → `documentation-index`.
Slugs must be unique across all of `documentation/` since `related_docs`/`superseded_by`
reference them without a path.

### `category` — one per directory

| category | directory | meaning |
|---|---|---|
| `architecture` | `Current_Architecture/`, `SOFTWARE_DESIGN.md`, `SOFTWARE_DESIGN_DIAGRAM.md` | Must match code now. `source_refs` required. |
| `reference` | `User_Reference/`, `POWER_MEASURMENTS.md` | Practical how-to or measurement reference. `source_refs` optional (only if it names specific build files/scripts). |
| `plan-pending` | `Pending_Plans/` | Scoped proposal awaiting implementation; use `status: draft`. No `source_refs` or `last_verified`. |
| `plan-possible` | `Possible_Plans/` | Concise restart note for deliberately deferred work. No `source_refs` or `last_verified`. |
| `plan-completed` | `Completed_Plans/`, `Project_Progress/` | Historical design record; code is authoritative, not this doc. No `last_verified` requirement. Use `superseded_by` if a `Current_Architecture` doc now fully covers what this plan describes. |
| `index` | `documentation/README.md` | Doc table of contents. `last_verified` meaningful (the list itself can go stale), no `source_refs`. |

### `status`

`current` (architecture/reference/index docs believed accurate) · `draft` (pending plans) · `deferred` (possible plans) ·
`historical` (completed plans — accurate as a record of intent, not of current code) ·
`superseded` (anything, of any category, known to be replaced/wrong but not yet deleted or
rewritten — a deliberate "don't trust this" flag, distinct from simply being old).

The restored `plan-pending` / `draft` metadata is not yet recognized by
`check_doc_freshness.py`. Updating that validator is recorded in the pending session
plan; this planning-only change does not modify Python code.

### `source_refs`

Repo-relative paths (from `SmartFires_IoT/`) to the code files this doc's claims are pinned
to. Be specific — list the files whose change should trigger a re-review of this doc, not
every file in the subsystem. This is the field `check_doc_freshness.py` actually uses.

### `last_verified`

The date someone last read this doc line-by-line against `source_refs` and confirmed it
matches. Update it whenever you do that review — including "I re-read it and it's still
right," not just when you fix something. A stale `last_verified` is itself a stale-doc bug.

## Freshness check

```bash
python3 documentation/check_doc_freshness.py
```

Validates required fields, path/category and category/status combinations, unique `name`
slugs, and `related_docs`/`superseded_by` targets, then compares
each current doc's `last_verified` against the date of the most recent commit touching each
of its `source_refs` (`git log -1 --format=%cI -- <path>`). Flags any doc where a referenced
source file was committed after the doc's last verification, and separately flags any
`source_refs` path that no longer exists (renamed/deleted file). This is a heuristic — it
catches "you touched the code and forgot the doc," not "you touched the doc correctly the
first time." Uncommitted working-tree changes to a source file won't trigger a flag until
they're committed.
