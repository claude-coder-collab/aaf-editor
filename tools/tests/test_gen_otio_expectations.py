from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

otio = pytest.importorskip("opentimelineio")

import gen_otio_expectations as gen


def rt(value: int) -> object:
    return otio.opentime.RationalTime(value, 24)


def test_describe_track_lists_items_in_frames() -> None:
    track = otio.schema.Track(name="V", kind=otio.schema.TrackKind.Video)
    track.append(otio.schema.Clip(name="a", source_range=otio.opentime.TimeRange(rt(0), rt(10))))
    track.append(otio.schema.Transition(in_offset=rt(2), out_offset=rt(3)))
    track.append(otio.schema.Gap(source_range=otio.opentime.TimeRange(rt(0), rt(5))))
    described = gen.describe_track(track)
    assert described["kind"] == "Video"
    assert described["items"] == [["Clip", 0, 10], ["Transition", None, 5], ["Gap", 10, 5]]


def test_committed_expectations_cover_the_fixtures() -> None:
    import json

    data = json.loads(gen.OUTPUT.read_text(encoding="utf-8"))
    assert len(data) >= 30
    assert all("timeline" in entry and "tracks" in entry for entry in data.values())
