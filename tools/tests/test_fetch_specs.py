from __future__ import annotations

import io
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import fetch_specs


def make_archive(members: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, data in members.items():
            zf.writestr(name, data)
    return buf.getvalue()


def test_extract_specs_flattens_names(tmp_path: Path) -> None:
    archive = make_archive({"AAF/doc/a.pdf": b"A", "AAF/doc/b.pdf": b"B", "AAF/other.txt": b"x"})
    written = fetch_specs.extract_specs(archive, tmp_path, ("AAF/doc/a.pdf", "AAF/doc/b.pdf"))
    assert [p.name for p in written] == ["a.pdf", "b.pdf"]
    assert (tmp_path / "a.pdf").read_bytes() == b"A"
    assert not (tmp_path / "other.txt").exists()


def test_main_rejects_hash_mismatch(tmp_path: Path) -> None:
    archive = tmp_path / "bad.zip"
    archive.write_bytes(make_archive({"x": b"y"}))
    assert fetch_specs.main(["--dest", str(tmp_path / "out"), "--archive", str(archive)]) == 1
