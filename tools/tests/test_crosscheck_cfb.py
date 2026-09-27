from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

aaf2 = pytest.importorskip("aaf2")

import crosscheck_cfb as cc

SDK_DIR = cc.REPO_ROOT / "tests" / "fixtures" / "aafsdk"


def test_reference_files_finds_sdk_fixtures() -> None:
    files = cc.reference_files([SDK_DIR, SDK_DIR / "missing"])
    assert len(files) == 6
    assert files == sorted(files)


def test_file_matches_itself() -> None:
    path = cc.reference_files([SDK_DIR])[0]
    assert cc.compare(path, path) is None
    snapshot = cc.cfb_snapshot(path)
    assert snapshot["/"][0] == "root"
    assert any(kind == "stream" for kind, _, _ in snapshot.values())
