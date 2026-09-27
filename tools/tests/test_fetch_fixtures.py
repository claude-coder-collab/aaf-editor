from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import fetch_fixtures as ff


def make_source(files: dict[str, bytes]) -> ff.Source:
    return ff.Source(
        name="demo",
        repo="owner/repo",
        commit="abc123",
        prefix="tests/data/",
        licence="MIT",
        files=[ff.FileEntry(path, ff.sha256(data), len(data)) for path, data in files.items()],
    )


def test_local_path_strips_prefix(tmp_path: Path) -> None:
    source = make_source({})
    assert source.local_path(tmp_path, "tests/data/sub/x.aaf") == tmp_path / "demo" / "sub" / "x.aaf"
    assert source.raw_url("tests/data/x.aaf") == "https://raw.githubusercontent.com/owner/repo/abc123/tests/data/x.aaf"


def test_manifest_round_trip(tmp_path: Path) -> None:
    source = make_source({"tests/data/a.aaf": b"a"})
    path = tmp_path / "manifest.json"
    path.write_text(ff.dump_manifest([source]))
    assert ff.load_manifest(path) == [source]


def test_fetch_downloads_verifies_and_skips(tmp_path: Path) -> None:
    content = {"tests/data/a.aaf": b"alpha", "tests/data/b/c.aaf": b"gamma"}
    source = make_source(content)
    calls: list[str] = []

    def download(url: str) -> bytes:
        calls.append(url)
        return content[url.split("/abc123/", 1)[1]]

    assert ff.fetch([source], tmp_path, download) == []
    assert (tmp_path / "demo" / "b" / "c.aaf").read_bytes() == b"gamma"
    assert len(calls) == 2
    assert ff.fetch([source], tmp_path, download) == []
    assert len(calls) == 2


def test_fetch_reports_hash_mismatch(tmp_path: Path) -> None:
    source = make_source({"tests/data/a.aaf": b"alpha"})
    errors = ff.fetch([source], tmp_path, lambda url: b"tampered")
    assert errors == ["demo/tests/data/a.aaf: SHA-256 mismatch"]
    assert not (tmp_path / "demo" / "a.aaf").exists()


def test_pin_lists_matching_files(tmp_path: Path) -> None:
    responses = {
        "https://api.github.com/repos/o/r/commits/main": json.dumps({"sha": "c0ffee"}).encode(),
        "https://api.github.com/repos/o/r/git/trees/c0ffee?recursive=1": json.dumps(
            {
                "tree": [
                    {"path": "t/x.AAF", "type": "blob"},
                    {"path": "t/readme.md", "type": "blob"},
                    {"path": "other/y.aaf", "type": "blob"},
                    {"path": "t/dir", "type": "tree"},
                ]
            }
        ).encode(),
        "https://raw.githubusercontent.com/o/r/c0ffee/t/x.AAF": b"data",
    }
    sources = ff.pin([ff.SourceDef("s", "o/r", "main", "t/", "MIT")], tmp_path, responses.__getitem__)
    assert [f.path for f in sources[0].files] == ["t/x.AAF"]
    assert sources[0].commit == "c0ffee"
    assert (tmp_path / "s" / "x.AAF").read_bytes() == b"data"


@pytest.mark.parametrize("command", ["fetch"])
def test_main_with_empty_manifest(tmp_path: Path, command: str) -> None:
    manifest = tmp_path / "m.json"
    manifest.write_text(ff.dump_manifest([]))
    assert ff.main([command, "--manifest", str(manifest), "--dest", str(tmp_path)]) == 0
