#!/usr/bin/env python3
"""Flag documentation/ files whose source_refs changed after last_verified.

See documentation/DOC_FRONTMATTER.md for the frontmatter schema this reads.
Run from anywhere inside the repo: python3 documentation/check_doc_freshness.py
"""
from __future__ import annotations

import subprocess
import sys
from datetime import date, datetime
from pathlib import Path

REPO_ROOT = Path(
    subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
)
DOCS_DIR = REPO_ROOT / "documentation"

# The schema definition describes the rules; it isn't itself a content doc pinned
# to source_refs, so it's exempt from the "must have frontmatter" check.
EXEMPT = {DOCS_DIR / "DOC_FRONTMATTER.md"}


def parse_frontmatter(text: str) -> dict | None:
    if not text.startswith("---\n"):
        return None
    end = text.find("\n---", 4)
    if end == -1:
        return None
    block = text[4:end]

    data: dict[str, object] = {}
    current_list_key: str | None = None
    for raw_line in block.splitlines():
        if not raw_line.strip():
            continue
        if raw_line.startswith("  - ") or raw_line.startswith("- "):
            if current_list_key is None:
                continue
            value = raw_line.split("- ", 1)[1].strip().strip('"').strip("'")
            data.setdefault(current_list_key, [])
            data[current_list_key].append(value)
            continue
        if ":" not in raw_line:
            continue
        key, _, value = raw_line.partition(":")
        key = key.strip()
        value = value.strip()
        if value == "" or value == "[]":
            current_list_key = key
            data.setdefault(key, [])
            continue
        current_list_key = None
        data[key] = value.strip('"').strip("'")
    return data


def last_commit_date(path: Path) -> date | None:
    result = subprocess.run(
        ["git", "log", "-1", "--format=%cI", "--", str(path)],
        cwd=REPO_ROOT, capture_output=True, text=True, check=True,
    )
    out = result.stdout.strip()
    if not out:
        return None
    return datetime.fromisoformat(out).date()


def main() -> int:
    problems: list[str] = []
    checked = 0

    for md_path in sorted(DOCS_DIR.rglob("*.md")):
        if md_path in EXEMPT:
            continue
        text = md_path.read_text()
        fm = parse_frontmatter(text)
        rel = md_path.relative_to(REPO_ROOT)
        if fm is None:
            problems.append(f"{rel}: no frontmatter found")
            continue

        checked += 1
        name = fm.get("name", "<missing name>")
        last_verified_raw = fm.get("last_verified")
        source_refs = fm.get("source_refs", [])

        if not source_refs:
            continue

        if not last_verified_raw:
            problems.append(
                f"{rel} ({name}): has source_refs but no last_verified date"
            )
            continue

        try:
            last_verified = date.fromisoformat(str(last_verified_raw))
        except ValueError:
            problems.append(
                f"{rel} ({name}): last_verified '{last_verified_raw}' is not YYYY-MM-DD"
            )
            continue

        for ref in source_refs:
            ref_path = REPO_ROOT / ref
            if not ref_path.exists():
                problems.append(
                    f"{rel} ({name}): source_ref '{ref}' does not exist on disk"
                )
                continue
            commit_date = last_commit_date(ref_path)
            if commit_date is None:
                continue  # uncommitted/new file — nothing to compare yet
            if commit_date > last_verified:
                problems.append(
                    f"{rel} ({name}): '{ref}' last committed {commit_date}, "
                    f"doc last_verified {last_verified} — review for drift"
                )

    print(f"Checked {checked} doc(s) with frontmatter under {DOCS_DIR.relative_to(REPO_ROOT)}/")
    if not problems:
        print("No staleness found.")
        return 0

    print(f"\n{len(problems)} issue(s):")
    for p in problems:
        print(f"  - {p}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
