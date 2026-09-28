from __future__ import annotations

import sys
from pathlib import Path

import aaf2
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gen_stress_aaf
import perf_baseline


def test_generate_builds_the_requested_composition(tmp_path: Path) -> None:
    path = tmp_path / "stress.aaf"
    total = gen_stress_aaf.generate(path, video=1, audio=2, clips=15, masters=4, seed=3)
    assert total == 45
    with aaf2.open(str(path), "r") as f:
        comps = [m for m in f.content.mobs if isinstance(m, aaf2.mobs.CompositionMob)]
        assert len(comps) == 1
        slots = list(comps[0].slots)
        assert [s.segment.media_kind for s in slots] == ["Picture", "Sound", "Sound"]
        for slot in slots:
            clips = [c for c in slot.segment.components if isinstance(c, aaf2.components.SourceClip)]
            assert len(clips) == 15
            assert slot.segment.length == sum(c.length for c in slot.segment.components)
        assert len([m for m in f.content.mobs if isinstance(m, aaf2.mobs.MasterMob)]) == 4


def test_generate_is_deterministic(tmp_path: Path) -> None:
    def lengths(path: Path) -> list[int]:
        with aaf2.open(str(path), "r") as f:
            comp = next(m for m in f.content.mobs if isinstance(m, aaf2.mobs.CompositionMob))
            return [c.length for s in comp.slots for c in s.segment.components]

    gen_stress_aaf.generate(tmp_path / "a.aaf", 1, 1, 30, 3, seed=9)
    gen_stress_aaf.generate(tmp_path / "b.aaf", 1, 1, 30, 3, seed=9)
    assert lengths(tmp_path / "a.aaf") == lengths(tmp_path / "b.aaf")


def test_report_formats_samples() -> None:
    text = perf_baseline.report([perf_baseline.Sample("doc.open", 0.0125, 1234)])
    lines = text.splitlines()
    assert lines[0].split() == ["stage", "ms", "response"]
    assert lines[1].split() == ["doc.open", "12.5", "1,234"]


@pytest.mark.skipif(not (Path(__file__).resolve().parents[2] / "build").is_dir(), reason="needs a built aaftool")
def test_measure_runs_every_stage(tmp_path: Path) -> None:
    root = Path(__file__).resolve().parents[2]
    tools = sorted(root.glob("build/*/apps/aaftool/*/aaftool")) + sorted(root.glob("build/*/apps/aaftool/*/aaftool.exe"))
    if not tools:
        pytest.skip("no aaftool build found")
    path = tmp_path / "small.aaf"
    gen_stress_aaf.generate(path, 1, 1, 10, 2)
    names = [s.name for s in perf_baseline.measure(tools[0], path, tmp_path)]
    assert names == ["doc.open", "timeline.mobs", "timeline.get", "timeline.op split", "timeline.get after edit", "timeline.get changed only", "edit.undo", "doc.validate", "doc.saveAs"]


def test_check_limits_reports_slow_large_and_missing_stages() -> None:
    samples = [perf_baseline.Sample("a", 0.2, 100), perf_baseline.Sample("b", 0.01, 5000)]
    problems = perf_baseline.check_limits(samples, {"a": {"ms": 100}, "b": {"ms": 100, "bytes": 1000}, "c": {"ms": 1}})
    assert problems == ["a: 200.0 ms > 100 ms", "b: 5,000 bytes > 1,000 bytes", "c: not measured"]
    assert perf_baseline.check_limits(samples, {"a": {"ms": 500, "bytes": 100}}) == []


def test_limits_file_names_every_measured_stage() -> None:
    import json

    limits = json.loads((Path(__file__).resolve().parents[1] / "perf_limits.json").read_text())
    assert set(limits["core"]) == {"doc.open", "timeline.mobs", "timeline.get", "timeline.op split", "timeline.get after edit", "timeline.get changed only", "edit.undo", "doc.validate", "doc.saveAs"}
    assert set(limits["file"]) == {"video", "audio", "clips", "masters", "seed"}

