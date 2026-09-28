from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import release_notes

CHANGELOG = """# Changelog

## 1.1.0

### Fixes

- Fixed a thing.

## 1.0.0

First.
"""


def test_section_is_extracted() -> None:
    assert release_notes.changelog_section(CHANGELOG, "1.1.0") == "### Fixes\n\n- Fixed a thing."
    assert release_notes.changelog_section(CHANGELOG, "1.0.0") == "First."


def test_missing_or_empty_section_is_an_error() -> None:
    with pytest.raises(ValueError):
        release_notes.changelog_section(CHANGELOG, "2.0.0")
    with pytest.raises(ValueError):
        release_notes.changelog_section("## 3.0.0\n\n## 2.0.0\nx\n", "3.0.0")


def test_render_puts_changes_after_the_intro() -> None:
    text = release_notes.render("Title @VERSION@.\n\n## Downloads\nfile-@VERSION@\n", CHANGELOG, "1.1.0")
    assert text.startswith("Title 1.1.0.\n\n## What's new\n\n### Fixes\n\n- Fixed a thing.\n")
    assert "file-1.1.0" in text
    assert "@VERSION@" not in text


def test_changelog_covers_the_project_version() -> None:
    version = release_notes.project_version((release_notes.REPO_ROOT / "CMakeLists.txt").read_text())
    assert release_notes.changelog_section((release_notes.REPO_ROOT / "CHANGELOG.md").read_text(), version)
    assert release_notes.main([f"v{version}"]) == 0


def test_project_version_is_read_from_cmake() -> None:
    assert release_notes.project_version('project(x VERSION 1.2.3 DESCRIPTION "d")') == "1.2.3"
