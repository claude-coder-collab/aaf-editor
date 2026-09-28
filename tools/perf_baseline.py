#!/usr/bin/env python3
"""Time the editor core's stages on one AAF file through `aaftool serve`.

Reports the time and response size of opening the file, listing mobs, projecting the largest composition,
a split in its middle track followed by a full and an incremental re-projection, undo, validation and saving.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class Sample:
    name: str
    seconds: float
    response_bytes: int


class Core:
    def __init__(self, aaftool: Path) -> None:
        self.process = subprocess.Popen([str(aaftool), "serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.next_id = 0
        self.samples: list[Sample] = []

    def call(self, name: str, method: str, params: dict[str, Any] | None = None) -> Any:
        assert self.process.stdin and self.process.stdout
        self.next_id += 1
        request = json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}})
        start = time.perf_counter()
        self.process.stdin.write(request + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        elapsed = time.perf_counter() - start
        reply = json.loads(line)
        response = reply["response"]
        if "error" in response:
            raise RuntimeError(f"{method}: {response['error']['message']}")
        self.samples.append(Sample(name, elapsed, len(line)))
        return response["result"]

    def close(self) -> None:
        if self.process.stdin:
            self.process.stdin.close()
        self.process.wait()


def measure(aaftool: Path, path: Path, workdir: Path) -> list[Sample]:
    core = Core(aaftool)
    try:
        core.call("doc.open", "doc.open", {"path": str(path)})
        mobs = core.call("timeline.mobs", "timeline.mobs")
        compositions = [m for m in mobs if m["kind"] == "composition"]
        if not compositions:
            return core.samples
        largest, timeline = None, None
        for mob in compositions:
            candidate = core.call(f"timeline.get ({mob['name']})", "timeline.get", {"mob": mob["id"]})
            if timeline is None or sum(len(t["items"]) for t in candidate["tracks"]) > sum(len(t["items"]) for t in timeline["tracks"]):
                largest, timeline = mob, candidate
        assert largest is not None and timeline is not None
        core.samples = [s for s in core.samples if not s.name.startswith("timeline.get") or s.name.endswith(f"({largest['name']})")]
        tracks = [t for t in timeline["tracks"] if t["slotKind"] == "timeline" and any(i["kind"] == "sourceClip" for i in t["items"])]
        if tracks:
            track = tracks[len(tracks) // 2]
            clip = next(i for i in track["items"][len(track["items"]) // 2 :] + track["items"] if i["kind"] == "sourceClip" and i["length"] > 1)
            split = core.call("timeline.op split", "timeline.op", {"op": "split", "slot": track["slot"], "position": clip["start"] + clip["length"] // 2})
            core.call("timeline.get after edit", "timeline.get", {"mob": largest["id"]})
            core.call("timeline.get changed only", "timeline.get", {"mob": largest["id"], "changed": split["changes"]["objects"]})
            core.call("edit.undo", "edit.undo")
        core.call("doc.validate", "doc.validate")
        core.call("doc.saveAs", "doc.saveAs", {"path": str(workdir / "saved.aaf")})
        return core.samples
    finally:
        core.close()


def report(samples: list[Sample]) -> str:
    lines = [f"{'stage':<32} {'ms':>9} {'response':>12}"]
    for s in samples:
        lines.append(f"{s.name:<32} {s.seconds * 1000:>9.1f} {s.response_bytes:>12,}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("aaftool", type=Path)
    parser.add_argument("file", type=Path)
    args = parser.parse_args(argv)
    with tempfile.TemporaryDirectory() as tmp:
        print(report(measure(args.aaftool, args.file, Path(tmp))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
