#!/usr/bin/env python3
"""Record how the OpenTimelineIO AAF adapter reads each external reference file.

Writes tests/fixtures/external/otio_expectations.json: for every file OTIO can read, the top-level timeline
name and, per video/audio track, the sequence of items as [type, start, duration] in frames. The C++ timeline
tests compare their projection with this. Requires `opentimelineio` and `otio-aaf-adapter`.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent
EXTERNAL = REPO_ROOT / "tests" / "fixtures" / "external"
OUTPUT = EXTERNAL / "otio_expectations.json"


def describe_track(track: Any) -> dict[str, Any]:
    import opentimelineio as otio

    items: list[list[Any]] = []
    for child in track:
        if isinstance(child, otio.schema.Transition):
            items.append(["Transition", None, child.in_offset.to_frames() + child.out_offset.to_frames()])
            continue
        kind = "Gap" if isinstance(child, otio.schema.Gap) else "Clip" if isinstance(child, otio.schema.Clip) else type(child).__name__
        start = child.range_in_parent().start_time.to_frames()
        items.append([kind, start, child.duration().to_frames()])
    return {"kind": track.kind, "name": track.name, "items": items}


def describe(path: Path) -> dict[str, Any] | None:
    import opentimelineio as otio

    try:
        timeline = otio.adapters.read_from_file(str(path))
    except Exception as error:
        print(f"skip {path.name}: {error}", file=sys.stderr)
        return None
    if isinstance(timeline, otio.schema.SerializableCollection):
        timelines = [t for t in timeline if isinstance(t, otio.schema.Timeline)]
        if not timelines:
            return None
        timeline = timelines[0]
    tracks = [t for t in timeline.tracks if isinstance(t, otio.schema.Track)]
    return {"timeline": timeline.name, "tracks": [describe_track(t) for t in tracks]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args(argv)
    result: dict[str, Any] = {}
    for path in sorted((EXTERNAL / "otio-aaf-adapter").glob("*.aaf")):
        described = describe(path)
        if described is not None:
            result[path.name] = described
    args.output.write_text(json.dumps(result, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {len(result)} timelines to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
