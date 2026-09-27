#!/usr/bin/env python3
"""Fetch the external AAF reference files listed in tests/fixtures/external/manifest.json.

`fetch` (the default) downloads missing files and verifies every SHA-256.
`pin` resolves each source's branch to a commit, lists its .aaf files and rewrites the manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import urllib.request
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

REPO_ROOT = Path(__file__).resolve().parent.parent
EXTERNAL_DIR = REPO_ROOT / "tests" / "fixtures" / "external"
MANIFEST = EXTERNAL_DIR / "manifest.json"

Downloader = Callable[[str], bytes]


@dataclass(frozen=True)
class SourceDef:
    name: str
    repo: str
    ref: str
    prefix: str
    licence: str


DEFAULT_SOURCES: tuple[SourceDef, ...] = (
    SourceDef("pyaaf2", "markreidvfx/pyaaf2", "main", "tests/test_files/", "MIT"),
    SourceDef("otio-aaf-adapter", "OpenTimelineIO/otio-aaf-adapter", "main", "tests/sample_data/", "Apache-2.0"),
)


@dataclass
class FileEntry:
    path: str
    sha256: str
    size: int


@dataclass
class Source:
    name: str
    repo: str
    commit: str
    prefix: str
    licence: str
    files: list[FileEntry] = field(default_factory=list)

    def raw_url(self, path: str) -> str:
        return f"https://raw.githubusercontent.com/{self.repo}/{self.commit}/{path}"

    def local_path(self, root: Path, path: str) -> Path:
        relative = PurePosixPath(path).relative_to(self.prefix)
        return root / self.name / Path(*relative.parts)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def http_get(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "aaf-editor-fixtures"})
    token = os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def load_manifest(path: Path) -> list[Source]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return [
        Source(
            name=s["name"],
            repo=s["repo"],
            commit=s["commit"],
            prefix=s["prefix"],
            licence=s["licence"],
            files=[FileEntry(**f) for f in s["files"]],
        )
        for s in data["sources"]
    ]


def dump_manifest(sources: Iterable[Source]) -> str:
    payload = {
        "sources": [
            {
                "name": s.name,
                "repo": s.repo,
                "commit": s.commit,
                "prefix": s.prefix,
                "licence": s.licence,
                "files": [{"path": f.path, "sha256": f.sha256, "size": f.size} for f in s.files],
            }
            for s in sources
        ]
    }
    return json.dumps(payload, indent=2) + "\n"


def fetch(sources: Iterable[Source], root: Path, download: Downloader = http_get) -> list[str]:
    """Ensures every manifest file exists under `root` with the right hash; returns error messages."""
    errors: list[str] = []
    for source in sources:
        for entry in source.files:
            target = source.local_path(root, entry.path)
            if target.exists() and sha256(target.read_bytes()) == entry.sha256:
                continue
            data = download(source.raw_url(entry.path))
            if sha256(data) != entry.sha256:
                errors.append(f"{source.name}/{entry.path}: SHA-256 mismatch")
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            print(f"fetched {target.relative_to(root)}")
    return errors


def pin(defs: Iterable[SourceDef], root: Path, download: Downloader = http_get) -> list[Source]:
    sources: list[Source] = []
    for d in defs:
        commit = json.loads(download(f"https://api.github.com/repos/{d.repo}/commits/{d.ref}"))["sha"]
        tree = json.loads(download(f"https://api.github.com/repos/{d.repo}/git/trees/{commit}?recursive=1"))
        paths = sorted(t["path"] for t in tree["tree"] if t["type"] == "blob" and t["path"].startswith(d.prefix) and t["path"].lower().endswith(".aaf"))
        source = Source(d.name, d.repo, commit, d.prefix, d.licence)
        for path in paths:
            data = download(source.raw_url(path))
            source.files.append(FileEntry(path, sha256(data), len(data)))
            target = source.local_path(root, path)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        sources.append(source)
    return sources


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", choices=("fetch", "pin"), default="fetch")
    parser.add_argument("--dest", type=Path, default=EXTERNAL_DIR)
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    args = parser.parse_args(argv)

    if args.command == "pin":
        sources = pin(DEFAULT_SOURCES, args.dest)
        args.manifest.write_text(dump_manifest(sources), encoding="utf-8")
        print(f"pinned {sum(len(s.files) for s in sources)} files to {args.manifest}")
        return 0

    errors = fetch(load_manifest(args.manifest), args.dest)
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
