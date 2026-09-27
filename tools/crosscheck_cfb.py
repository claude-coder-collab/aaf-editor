#!/usr/bin/env python3
"""Cross-check aaftool's compound-file writer against pyaaf2.

For every reference .aaf file, `aaftool cfb-roundtrip` rewrites it as v3 and v4, and pyaaf2 must read the
rewritten file with the same directory tree, stream bytes, CLSIDs and mobs as the original.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path
from typing import Any

import aaf2
import aaf2.cfb

REPO_ROOT = Path(__file__).resolve().parent.parent
FIXTURE_DIRS = (REPO_ROOT / "tests" / "fixtures" / "aafsdk", REPO_ROOT / "tests" / "fixtures" / "external")

Snapshot = dict[str, tuple[str, str, bytes | None]]


def reference_files(dirs: Iterable[Path]) -> list[Path]:
    return sorted(p for d in dirs if d.is_dir() for p in d.rglob("*") if p.suffix.lower() == ".aaf")


def cfb_snapshot(path: Path) -> Snapshot:
    snapshot: Snapshot = {}
    with path.open("rb") as f:
        cfb = aaf2.cfb.CompoundFileBinary(f, "rb")
        pending: list[Any] = [cfb.find("/")]
        while pending:
            entry = pending.pop()
            for child in cfb.listdir(entry):
                data = None if child.isdir() else child.open("r").read()
                snapshot[child.path()] = ("storage" if child.isdir() else "stream", str(child.class_id), data)
                if child.isdir():
                    pending.append(child)
        snapshot["/"] = ("root", str(cfb.find("/").class_id), None)
    return snapshot


def mob_ids(path: Path) -> list[str]:
    with aaf2.open(str(path), "r") as f:
        return sorted(str(m.mob_id) for m in f.content.mobs)


def compare(original: Path, rewritten: Path) -> str | None:
    a, b = cfb_snapshot(original), cfb_snapshot(rewritten)
    if a.keys() != b.keys():
        return f"entries differ: {sorted(a.keys() ^ b.keys())[:5]}"
    for key in a:
        if a[key] != b[key]:
            return f"{key} differs"
    if mob_ids(original) != mob_ids(rewritten):
        return "mob IDs differ"
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("aaftool", type=Path)
    args = parser.parse_args(argv)

    files = reference_files(FIXTURE_DIRS)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path in files:
            for version in ("--v3", "--v4"):
                out = Path(tmp) / "out.aaf"
                run = subprocess.run([str(args.aaftool), "cfb-roundtrip", str(path), str(out), version], capture_output=True, text=True)
                problem = run.stderr.strip() if run.returncode != 0 else compare(path, out)
                if problem:
                    failures += 1
                    print(f"FAIL {path.relative_to(REPO_ROOT)} {version}: {problem}")
    print(f"{len(files)} files, {failures} failures")
    return 1 if failures or not files else 0


if __name__ == "__main__":
    sys.exit(main())
