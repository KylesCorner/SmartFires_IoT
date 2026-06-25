#!/usr/bin/env python3
"""Check platformio/include + platformio/src C++ file headers against documentation/*.md.

See documentation/CODE_FRONTMATTER.md for the header schema and
documentation/DOC_FRONTMATTER.md for the doc-side source_refs schema this cross-checks
against. Run from anywhere inside the repo: python3 documentation/check_code_headers.py
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(
    subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
)
DOCS_DIR = REPO_ROOT / "documentation"
CODE_DIRS = [REPO_ROOT / "platformio" / "include", REPO_ROOT / "platformio" / "src"]
VALID_ROLES = {"interface", "implementation", "config", "entrypoint"}


def parse_doc_source_refs() -> dict[str, set[str]]:
    """doc slug -> set of repo-relative source_refs paths."""
    result: dict[str, set[str]] = {}
    for md in sorted(DOCS_DIR.rglob("*.md")):
        text = md.read_text()
        if not text.startswith("---\n"):
            continue
        end = text.find("\n---", 4)
        block = text[4:end]
        name = None
        refs: set[str] = set()
        in_refs = False
        for line in block.splitlines():
            if line.startswith("name:"):
                name = line.split(":", 1)[1].strip()
            if line.strip() == "source_refs:":
                in_refs = True
                continue
            if in_refs:
                if line.startswith("  - "):
                    refs.add(line.split("- ", 1)[1].strip())
                else:
                    in_refs = False
        if name:
            result[name] = refs
    return result


def parse_code_header(text: str) -> dict | None:
    if not text.startswith("// ---\n"):
        return None
    end = text.find("\n// ---", 7)
    if end == -1:
        return None
    block = text[7:end]

    data: dict[str, object] = {}
    for raw_line in block.splitlines():
        line = raw_line[3:] if raw_line.startswith("// ") else raw_line.lstrip("/").strip()
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        key = key.strip()
        value = value.strip()
        if value.startswith("[") and value.endswith("]"):
            inner = value[1:-1].strip()
            data[key] = [v.strip() for v in inner.split(",")] if inner else []
        else:
            data[key] = value
    return data


def main() -> int:
    doc_refs = parse_doc_source_refs()
    problems: list[str] = []
    checked = 0

    code_files: list[Path] = []
    for d in CODE_DIRS:
        code_files.extend(sorted(d.rglob("*.h")))
        code_files.extend(sorted(d.rglob("*.cpp")))

    for path in code_files:
        rel = path.relative_to(REPO_ROOT)
        text = path.read_text()
        header = parse_code_header(text)
        if header is None:
            problems.append(f"{rel}: no header found")
            continue

        checked += 1
        rel_str = str(rel)

        role = header.get("role")
        if role not in VALID_ROLES:
            problems.append(f"{rel}: role '{role}' is not one of {sorted(VALID_ROLES)}")

        if "description" not in header or not str(header["description"]).strip():
            problems.append(f"{rel}: missing description")

        declared_docs = set(header.get("docs", []) or [])
        expected_docs = {slug for slug, refs in doc_refs.items() if rel_str in refs}

        missing = expected_docs - declared_docs
        extra = declared_docs - expected_docs
        if missing:
            problems.append(
                f"{rel}: docs missing {sorted(missing)} — those docs list this file in "
                f"source_refs but the header doesn't list them back"
            )
        if extra:
            problems.append(
                f"{rel}: docs claims {sorted(extra)} but those docs' source_refs don't "
                f"include this file — remove from header or add to the doc's source_refs"
            )

    print(f"Checked {checked} code file(s) under platformio/include and platformio/src")
    if not problems:
        print("No issues found.")
        return 0

    print(f"\n{len(problems)} issue(s):")
    for p in problems:
        print(f"  - {p}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
