#!/usr/bin/env python3
"""Write the GitHub release notes for a version: the packaging template, with that version's CHANGELOG section."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def changelog_section(changelog: str, version: str) -> str:
    """Returns the body of the `## <version>` section."""
    match = re.search(rf"^## {re.escape(version)}\s*$(.*?)(?=^## |\Z)", changelog, re.MULTILINE | re.DOTALL)
    if not match or not match.group(1).strip():
        raise ValueError(f"CHANGELOG.md has no section for {version}")
    return match.group(1).strip()


def render(template: str, changelog: str, version: str) -> str:
    intro, _, rest = template.replace("@VERSION@", version).partition("\n")
    return f"{intro}\n\n## What's new\n\n{changelog_section(changelog, version)}\n{rest}"


def project_version(cmake: str) -> str:
    match = re.search(r"project\([^)]*VERSION\s+([0-9.]+)", cmake)
    if not match:
        raise ValueError("no project VERSION in CMakeLists.txt")
    return match.group(1)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="for example 0.2.0 or v0.2.0")
    args = parser.parse_args(argv)
    version = args.version.removeprefix("v")
    template = (REPO_ROOT / "packaging" / "RELEASE_NOTES.md").read_text()
    changelog = (REPO_ROOT / "CHANGELOG.md").read_text()
    try:
        sys.stdout.write(render(template, changelog, version))
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
