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
EXEMPT = {
    DOCS_DIR / "DOC_FRONTMATTER.md",
    DOCS_DIR / "CODE_FRONTMATTER.md",
}
VALID_CATEGORIES = {"architecture", "reference", "plan-possible", "plan-completed", "index"}
VALID_STATUSES = {"current", "deferred", "historical", "superseded"}


def expected_category(md_path: Path) -> str | None:
    """Return the schema category implied by a documentation path."""
    rel = md_path.relative_to(DOCS_DIR)
    if rel == Path("README.md"):
        return "index"
    if rel in {Path("SOFTWARE_DESIGN.md"), Path("SOFTWARE_DESIGN_DIAGRAM.md")}:
        return "architecture"
    if rel == Path("POWER_MEASURMENTS.md"):
        return "reference"
    if rel.parts[0] == "Current_Architecture":
        return "architecture"
    if rel.parts[0] == "User_Reference":
        return "reference"
    if rel.parts[0] == "Possible_Plans":
        return "plan-possible"
    if rel.parts[0] in {"Completed_Plans", "Project_Progress"}:
        return "plan-completed"
    return None


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
    records: list[tuple[Path, dict]] = []

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
        records.append((md_path, fm))
        name = fm.get("name", "<missing name>")
        category = fm.get("category")
        status = fm.get("status")
        last_verified_raw = fm.get("last_verified")
        source_refs = fm.get("source_refs", [])

        for required in ("name", "description", "category", "status"):
            if not fm.get(required):
                problems.append(f"{rel}: missing required '{required}' field")

        if category not in VALID_CATEGORIES:
            problems.append(
                f"{rel} ({name}): category '{category}' is not one of {sorted(VALID_CATEGORIES)}"
            )
        expected = expected_category(md_path)
        if expected is not None and category != expected:
            problems.append(
                f"{rel} ({name}): category '{category}' does not match path (expected '{expected}')"
            )
        if status not in VALID_STATUSES:
            problems.append(
                f"{rel} ({name}): status '{status}' is not one of {sorted(VALID_STATUSES)}"
            )
        if category == "plan-possible" and status != "deferred":
            problems.append(f"{rel} ({name}): possible plans must use status 'deferred'")
        if category == "plan-completed" and status not in {"historical", "superseded"}:
            problems.append(
                f"{rel} ({name}): completed/history docs must be historical or superseded"
            )
        if category in {"architecture", "reference", "index"} and not last_verified_raw:
            problems.append(
                f"{rel} ({name}): category '{category}' requires last_verified"
            )
        if category == "architecture" and not source_refs:
            problems.append(f"{rel} ({name}): architecture docs require source_refs")
        if category in {"plan-possible", "plan-completed"} and source_refs:
            problems.append(
                f"{rel} ({name}): plans/history must not declare source_refs"
            )

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

    # Slugs are the foreign keys used by related_docs and superseded_by. Check
    # them after the per-file pass so forward references are valid.
    name_paths: dict[str, list[Path]] = {}
    for md_path, fm in records:
        name = fm.get("name")
        if isinstance(name, str) and name:
            name_paths.setdefault(name, []).append(md_path)

    for name, paths in sorted(name_paths.items()):
        if len(paths) > 1:
            joined = ", ".join(str(path.relative_to(REPO_ROOT)) for path in paths)
            problems.append(f"duplicate name slug '{name}': {joined}")

    known_names = set(name_paths)
    for md_path, fm in records:
        rel = md_path.relative_to(REPO_ROOT)
        name = fm.get("name", "<missing name>")
        related = fm.get("related_docs", [])
        if isinstance(related, str):
            related = [related]
        if not isinstance(related, list):
            problems.append(f"{rel} ({name}): related_docs must be a list")
            related = []
        for target in related:
            if target not in known_names:
                problems.append(
                    f"{rel} ({name}): related_docs target '{target}' does not exist"
                )

        superseded_by = fm.get("superseded_by")
        if superseded_by in {None, "null", "None", "~"}:
            continue
        if not isinstance(superseded_by, str) or superseded_by not in known_names:
            problems.append(
                f"{rel} ({name}): superseded_by target '{superseded_by}' does not exist"
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
