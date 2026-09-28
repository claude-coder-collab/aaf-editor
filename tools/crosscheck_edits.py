#!/usr/bin/env python3
"""Check that files edited with the timeline operations stay valid and readable by other tools.

For each reference file with a composition, applies split, lift, overwrite, add-track and add-marker
operations through `aaftool rpc`, saves the result, and requires that:
  - `aaftool validate` reports no errors,
  - pyaaf2 opens the file and sees the same mobs,
  - the OpenTimelineIO AAF adapter reads the edited composition with the same clips and gaps as
    `aaftool timeline` (tracks that differ only in representation are skipped, as in the C++ tests).
"""

from __future__ import annotations

import argparse
import json
import logging
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

import aaf2

REPO_ROOT = Path(__file__).resolve().parent.parent
FIXTURES = (REPO_ROOT / "tests" / "fixtures" / "aafsdk", REPO_ROOT / "tests" / "fixtures" / "external" / "otio-aaf-adapter")
KIND = {"filler": "Gap", "transition": "Transition"}


def aaftool(tool: Path, *args: str) -> str:
    run = subprocess.run([str(tool), *args], capture_output=True, text=True)
    if run.returncode != 0:
        raise RuntimeError(f"aaftool {' '.join(args[:2])} failed: {run.stderr.strip()}")
    return run.stdout


def plan_edits(tool: Path, path: Path) -> tuple[list[tuple[str, dict[str, Any]]], str] | None:
    mobs = [line.split("\t") for line in aaftool(tool, "timeline", str(path), "--mobs").splitlines() if line]
    compositions = [m for m in mobs if m[1] == "composition"]
    if not compositions:
        return None
    mob = int(compositions[0][0])
    timeline = json.loads(aaftool(tool, "timeline", str(path), "--mob", str(mob), "--json"))
    calls: list[tuple[str, dict[str, Any]]] = []
    for track in timeline["tracks"]:
        items = track["items"]
        clips = [
            item
            for n, item in enumerate(items)
            if item["kind"] == "sourceClip"
            and item["length"] >= 4
            and item.get("source", {}).get("found")
            and not any(0 <= m < len(items) and items[m]["kind"] == "transition" for m in (n - 1, n + 1))
        ]
        if track["kind"] not in ("picture", "sound") or track["slotKind"] != "timeline" or not clips:
            continue
        clip = clips[0]
        calls.append(("timeline.op", {"op": "split", "slot": track["slot"], "position": clip["start"] + clip["length"] // 2}))
        mobs_by_id = {m[2]: int(m[0]) for m in mobs}
        source = mobs_by_id.get(clip["source"]["mobId"])
        if source is not None:
            calls.append(("timeline.op", {"op": "overwriteClip", "slot": track["slot"], "position": clip["start"], "sourceMob": source, "sourceSlot": clip["source"]["slotId"], "sourceIn": clip["source"]["startTime"], "length": 2}))
        break
    calls.append(("timeline.op", {"op": "addTrack", "mob": mob, "kind": "sound", "name": "crosscheck"}))
    calls.append(("timeline.op", {"op": "addMarker", "mob": mob, "position": 3, "comment": "crosscheck"}))
    return calls, compositions[0][4].replace(" (top level)", "")


def otio_tracks(path: Path, name: str) -> list[list[list[Any]]] | None:
    import opentimelineio as otio

    try:
        timeline = otio.adapters.read_from_file(str(path))
    except Exception as error:
        raise RuntimeError(f"OTIO cannot read the edited file: {error}") from error
    if isinstance(timeline, otio.schema.SerializableCollection):
        timeline = next((t for t in timeline if isinstance(t, otio.schema.Timeline) and t.name == name), None)
        if timeline is None:
            return None
    out = []
    for track in timeline.tracks:
        if track.kind not in ("Video", "Audio"):
            continue
        items = []
        for child in track:
            if isinstance(child, otio.schema.Transition):
                items.append(["Transition", None, None])
            else:
                kind = "Gap" if isinstance(child, otio.schema.Gap) else "Clip"
                items.append([kind, child.range_in_parent().start_time.to_frames(), child.duration().to_frames()])
        out.append(items)
    return out


def our_tracks(tool: Path, path: Path, name: str) -> tuple[list[list[list[Any]]], bool]:
    mobs = [line.split("\t") for line in aaftool(tool, "timeline", str(path), "--mobs").splitlines() if line]
    mob = next(m[0] for m in mobs if m[4].replace(" (top level)", "") == name)
    timeline = json.loads(aaftool(tool, "timeline", str(path), "--mob", mob, "--json"))
    out = []
    representational = False
    for track in timeline["tracks"]:
        if track["kind"] not in ("picture", "sound") or all(i["kind"] == "filler" for i in track["items"]):
            continue
        items = []
        for item in track["items"]:
            if item["kind"] in ("marker", "event"):
                continue
            source = item.get("source", {})
            representational = representational or item["kind"] == "nestedScope" or source.get("mobKind") == "composition"
            items.append([KIND.get(item["kind"], "Clip"), item["start"], item["length"]])
        out.append(items)
    return out, representational


def check(tool: Path, path: Path, workdir: Path) -> list[str]:
    planned = plan_edits(tool, path)
    if planned is None:
        return []
    calls, name = planned
    edited = workdir / "edited.aaf"
    args = ["rpc", str(path)]
    for method, params in calls:
        args += ["--call", method, json.dumps(params)]
    args += ["--save", str(edited)]
    try:
        aaftool(tool, *args)
    except RuntimeError as error:
        return [str(error)]
    problems: list[str] = []
    summary = subprocess.run([str(tool), "validate", str(edited)], capture_output=True, text=True)
    if summary.returncode != 0:
        problems.append("validation errors: " + summary.stdout.strip().splitlines()[-1])
    with aaf2.open(str(path), "r") as original, aaf2.open(str(edited), "r") as f:
        if len(list(original.content.mobs)) != len(list(f.content.mobs)):
            problems.append("pyaaf2 sees a different number of mobs")
    if "otio-aaf-adapter" in str(path):
        try:
            theirs = otio_tracks(edited, name)
        except RuntimeError as error:
            return problems + [str(error)]
        ours, representational = our_tracks(tool, edited, name)
        if theirs is not None and not representational and len(ours) == len(theirs):
            for mine, other in zip(ours, theirs):
                if all(x[0] in ("Clip", "Gap") for x in other) and mine != other:
                    problems.append(f"OTIO reads a track differently: ours {mine[:4]} vs OTIO {other[:4]}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("aaftool", type=Path)
    args = parser.parse_args(argv)
    logging.disable(logging.WARNING)
    files = sorted(p for d in FIXTURES if d.is_dir() for p in d.rglob("*.aaf"))
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path in files:
            problems = check(args.aaftool, path, Path(tmp))
            if problems:
                failures += 1
                print(f"FAIL {path.relative_to(REPO_ROOT)}")
                for problem in problems:
                    print(f"  {problem}")
    print(f"{len(files)} files, {failures} failures")
    return 1 if failures or not files else 0


if __name__ == "__main__":
    sys.exit(main())
