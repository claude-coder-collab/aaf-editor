#!/usr/bin/env python3
"""Generate a large synthetic AAF composition for performance measurements.

The composition has `--video` picture tracks and `--audio` sound tracks, each a sequence of `--clips` source clips
(with an occasional filler) that reference a pool of master mobs. Output is deterministic for a given seed.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

import aaf2

EDIT_RATE = "25"
MASTER_LENGTH = 100_000


def generate(path: Path, video: int, audio: int, clips: int, masters: int, seed: int = 1) -> int:
    """Writes the file and returns the number of clips in the composition."""
    rng = random.Random(seed)
    total = 0
    with aaf2.open(str(path), "w") as f:
        pool = []
        for n in range(masters):
            master = f.create.MasterMob(f"Master {n:04d}")
            f.content.mobs.append(master)
            for kind in ("picture", "sound"):
                slot = master.create_timeline_slot(EDIT_RATE)
                slot.segment = f.create.SourceClip(media_kind=kind, length=MASTER_LENGTH)
            pool.append(master)
        comp = f.create.CompositionMob("Stress")
        comp.usage = "Usage_TopLevel"
        f.content.mobs.append(comp)
        for kind, count in (("picture", video), ("sound", audio)):
            for _ in range(count):
                slot = comp.create_timeline_slot(EDIT_RATE)
                sequence = f.create.Sequence(media_kind=kind)
                slot.segment = sequence
                for _ in range(clips):
                    if rng.random() < 0.05:
                        sequence.components.append(f.create.Filler(kind, rng.randint(1, 50)))
                    master = rng.choice(pool)
                    source_slot = 1 if kind == "picture" else 2
                    length = rng.randint(12, 250)
                    start = rng.randint(0, MASTER_LENGTH - length)
                    sequence.components.append(master.create_source_clip(source_slot, start, length, kind))
                    total += 1
                sequence.length = sum(c.length for c in sequence.components)
    return total


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=Path)
    parser.add_argument("--video", type=int, default=2)
    parser.add_argument("--audio", type=int, default=28)
    parser.add_argument("--clips", type=int, default=2000, help="clips per track")
    parser.add_argument("--masters", type=int, default=500)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args(argv)
    total = generate(args.output, args.video, args.audio, args.clips, args.masters, args.seed)
    print(f"{args.output}: {args.video + args.audio} tracks, {total} clips")
    return 0


if __name__ == "__main__":
    sys.exit(main())
