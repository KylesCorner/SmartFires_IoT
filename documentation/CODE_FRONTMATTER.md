# Code File Header Schema

Companion to [DOC_FRONTMATTER.md](DOC_FRONTMATTER.md). That schema lets `documentation/*.md`
files declare which code they describe (`source_refs`) and be checked against it
(`last_verified` vs. git history). Code files don't have an independent "ground truth" to
check a header against — the code *is* the ground truth — so this schema is deliberately
narrower. It does not include `last_verified`, `depends_on`, or `used_by`: those fields would
be hand-maintained claims sitting next to the code they describe, with no way to detect when
they drift, which is exactly the failure mode `DOC_FRONTMATTER.md` exists to fix elsewhere.

Currently scoped to `platformio/include/` and `platformio/src/` (the C++ firmware). Not yet
applied to the edge-receiver Python package, `util/` scripts, or web dashboard assets.

## Format

A `//`-comment block, `---`-delimited, as the very first lines of the file — above
`#pragma once`, above any existing descriptive comment (don't delete those; this block adds a
machine-checkable summary, it doesn't replace prose explanation):

```cpp
// ---
// description: One-line purpose of this file.
// role: config
// docs: [tdma-protocol, packet-reliability]
// ---
#pragma once
```

## Fields

- **description** (required) — one line, what this file is for. If the file already starts
  with a good explanatory comment, summarize it; don't just repeat the first sentence verbatim.
- **role** (required) — one of:
  | role | meaning |
  |---|---|
  | `interface` | Pure abstract interface (`I*.h`) — no concrete behavior. |
  | `implementation` | Concrete logic: a class, driver, sensor, or service with behavior. |
  | `config` | Tunable constants / config structs only — no logic (`include/config/*`, `TdmaConfig.h`). |
  | `entrypoint` | Has `setup()`/`loop()` or `main()` — where firmware execution starts (`main*.cpp`). |
- **docs** (optional, omit if empty — don't write `docs: []`) — list of `documentation/`
  doc slugs (from `DOC_FRONTMATTER.md`'s `name` field) that describe this file. **This list
  must exactly match the set of docs whose own `source_refs` includes this file's path** —
  it's a backlink, not an independent judgment call. Don't add a doc here unless that doc's
  frontmatter already lists this file in `source_refs` (or you're updating both in the same
  change). `check_code_headers.py` enforces this both directions.

## Why no `name` field

Docs need a slug because `related_docs`/`superseded_by` link docs to each other without a
path. Code files don't need that — the file path already is the stable identifier nothing
else points to indirectly.

## Checking

```bash
python3 documentation/check_code_headers.py
```

Flags: files with no header, malformed headers, and any asymmetry between a file's `docs:`
list and the matching docs' `source_refs` lists (each side committed at a different time, so
asymmetry usually means one side wasn't updated when the other was).
