#!/usr/bin/env python3
"""Cross-check `aaftool dump --json --header` against pyaaf2's reading of the same file.

With `--roundtrip`, each file is first rewritten by `aaftool roundtrip` (preserved layout as v3, and
regenerated layout as v4), and pyaaf2's reading of the rewritten file is compared with aaftool's
reading of the original.

Both sides are reduced to the canonical JSON form described in SPEC §8.1; strong-reference sets are
compared order-insensitively. Differences are printed as JSON paths.
"""

from __future__ import annotations

import argparse
import datetime
import json
import logging
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path
from typing import Any

import aaf2
from aaf2 import properties as props
from aaf2.auid import AUID
from aaf2.mobid import MobID
from aaf2.rational import AAFRational

REPO_ROOT = Path(__file__).resolve().parent.parent
FIXTURE_DIRS = (REPO_ROOT / "tests" / "fixtures" / "aafsdk", REPO_ROOT / "tests" / "fixtures" / "external")

Json = Any


def reference_files(dirs: Iterable[Path]) -> list[Path]:
    return sorted(p for d in dirs if d.is_dir() for p in d.rglob("*") if p.suffix.lower() == ".aaf")


def canon_value(v: Any) -> Json:
    if v is None or isinstance(v, (bool, str)):
        return v
    if isinstance(v, int):
        return v
    if isinstance(v, (AUID, MobID)):
        return v.urn if isinstance(v, MobID) else str(v)
    if isinstance(v, AAFRational):
        return {"Numerator": v.numerator, "Denominator": v.denominator}
    if isinstance(v, datetime.datetime):
        return {"date": canon_value(v.date()), "time": canon_value(v.time())}
    if isinstance(v, datetime.date):
        return {"year": v.year, "month": v.month, "day": v.day}
    if isinstance(v, datetime.time):
        return {"hour": v.hour, "minute": v.minute, "second": v.second, "fraction": v.microsecond}
    if isinstance(v, dict):
        return {str(k): canon_value(x) for k, x in v.items()}
    if isinstance(v, (set, frozenset)):
        return sorted((canon_value(x) for x in v), key=sort_key)
    if isinstance(v, (list, tuple)):
        return [canon_value(x) for x in v]
    if isinstance(v, (bytes, bytearray)):
        return bytes(v).hex()
    return repr(v)


def canon_key(key: Any) -> Json:
    return key.urn if isinstance(key, MobID) else str(key)


def canon_property(p: Any) -> Json:
    if isinstance(p, props.StrongRefProperty):
        return canon_object(p.value)
    if isinstance(p, (props.StrongRefVectorProperty, props.StrongRefSetProperty)):
        return [canon_object(o) for o in p.value]
    if isinstance(p, props.WeakRefArrayProperty):
        return [canon_key(k) for k in p.references]
    if isinstance(p, props.WeakRefProperty):
        return canon_key(p.ref)
    if isinstance(p, props.StreamProperty):
        entry = p.parent.dir.get(p.stream_name)
        return {"stream": p.stream_name, "size": entry.byte_size if entry is not None else 0}
    return canon_value(p.value)


def canon_object(obj: Any) -> Json:
    return {"class": obj.classdef.class_name, "properties": {p.name: canon_property(p) for p in obj.properties()}}


def sort_key(v: Json) -> str:
    return json.dumps(v, sort_keys=True)


def is_zero_rational(v: Json) -> bool:
    """pyaaf2 normalises a stored 0/0 rational to 0/1, so any zero rational compares equal."""
    return isinstance(v, dict) and set(v) == {"Numerator", "Denominator"} and v["Numerator"] == 0


def diff(a: Json, b: Json, path: str, out: list[str], ignore_stream_names: bool = False) -> None:
    if len(out) >= 20:
        return
    if is_zero_rational(a) and is_zero_rational(b):
        return
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                out.append(f"{path}/{key}: only in {'aaftool' if key in a else 'pyaaf2'}")
                continue
            if ignore_stream_names and key == "stream" and "size" in a:
                continue
            diff(a[key], b[key], f"{path}/{key}", out, ignore_stream_names)
        return
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(f"{path}: length {len(a)} vs {len(b)}")
            return
        if a and isinstance(a[0], dict) and "class" in a[0]:
            a, b = sorted(a, key=sort_key), sorted(b, key=sort_key)
        for i, (x, y) in enumerate(zip(a, b)):
            diff(x, y, f"{path}[{i}]", out, ignore_stream_names)
        return
    if a != b and not (isinstance(a, (int, float)) and isinstance(b, (int, float)) and a == b):
        out.append(f"{path}: {json.dumps(a)[:80]} vs {json.dumps(b)[:80]}")


ROUNDTRIP_MODES: tuple[tuple[str, ...], ...] = (("--v3",), ("--v4", "--regenerate-layout"))


def compare(aaftool: Path, path: Path, pyaaf2_path: Path | None = None, ignore_stream_names: bool = False) -> list[str]:
    run = subprocess.run([str(aaftool), "dump", str(path), "--json", "--header"], capture_output=True, text=True)
    if run.returncode != 0:
        return [f"aaftool failed: {run.stderr.strip()}"]
    ours = json.loads(run.stdout)
    with aaf2.open(str(pyaaf2_path or path), "r") as f:
        theirs = canon_object(f.header)
    problems: list[str] = []
    diff(ours, theirs, "", problems, ignore_stream_names)
    return problems


def compare_roundtrip(aaftool: Path, path: Path, workdir: Path) -> list[str]:
    problems: list[str] = []
    for mode in ROUNDTRIP_MODES:
        out = workdir / "roundtrip.aaf"
        run = subprocess.run([str(aaftool), "roundtrip", str(path), str(out), *mode], capture_output=True, text=True)
        if run.returncode != 0:
            problems.append(f"{' '.join(mode)}: aaftool roundtrip failed: {run.stderr.strip()}")
            continue
        regenerated = "--regenerate-layout" in mode
        problems += [f"{' '.join(mode)}: {p}" for p in compare(aaftool, path, out, regenerated)]
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("aaftool", type=Path)
    parser.add_argument("files", nargs="*", type=Path)
    parser.add_argument("--roundtrip", action="store_true", help="compare pyaaf2's reading of files rewritten by aaftool")
    args = parser.parse_args(argv)
    logging.disable(logging.WARNING)

    files = args.files or reference_files(FIXTURE_DIRS)
    failures = 0
    workdir = Path(tempfile.mkdtemp())
    for path in files:
        problems = compare_roundtrip(args.aaftool, path, workdir) if args.roundtrip else compare(args.aaftool, path)
        if problems:
            failures += 1
            print(f"FAIL {path}")
            for problem in problems:
                print(f"  {problem}")
    shutil.rmtree(workdir, ignore_errors=True)
    print(f"{len(files)} files, {failures} failures")
    return 1 if failures or not files else 0


if __name__ == "__main__":
    sys.exit(main())
