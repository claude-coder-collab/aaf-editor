#!/usr/bin/env python3
"""Download the AAF specification PDFs (shipped inside the AAF SDK 1.2.0 archive) into docs/reference/."""

from __future__ import annotations

import argparse
import hashlib
import io
import sys
import urllib.request
import zipfile
from pathlib import Path

ARCHIVE_URL = "https://sourceforge.net/projects/aaf/files/AAF-src/1.2.0/AAF-src-1.2.0.zip/download"
ARCHIVE_SHA256 = "4d833409fb7c5c7b3193c12264eec20abc363c430f8224af9f14fba8eacb07d6"
SPEC_FILES: tuple[str, ...] = (
    "AAF/doc/aafobjectspec-v1.1.pdf",
    "AAF/doc/aafobjectspec-v1.0.1.pdf",
    "AAF/doc/aafstoredformatspec-v1.0.1.pdf",
    "AAF/doc/aafcontainerspec-v1.0.1.pdf",
    "AAF/doc/aafeditprotocol.pdf",
)
DEFAULT_DEST = Path(__file__).resolve().parent.parent / "docs" / "reference"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def download(url: str) -> bytes:
    with urllib.request.urlopen(url) as response:
        return response.read()


def extract_specs(archive: bytes, dest: Path, members: tuple[str, ...] = SPEC_FILES) -> list[Path]:
    dest.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    with zipfile.ZipFile(io.BytesIO(archive)) as zf:
        for member in members:
            target = dest / Path(member).name
            target.write_bytes(zf.read(member))
            written.append(target)
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--archive", type=Path, help="use a local copy of AAF-src-1.2.0.zip instead of downloading")
    args = parser.parse_args(argv)

    if all((args.dest / Path(m).name).exists() for m in SPEC_FILES):
        print(f"specs already present in {args.dest}")
        return 0

    archive = args.archive.read_bytes() if args.archive else download(ARCHIVE_URL)
    digest = sha256(archive)
    if digest != ARCHIVE_SHA256:
        print(f"archive SHA-256 mismatch: {digest} != {ARCHIVE_SHA256}", file=sys.stderr)
        return 1
    for path in extract_specs(archive, args.dest):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
