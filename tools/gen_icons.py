#!/usr/bin/env python3
"""Draw the AAF Editor application icon and write it in every format the packages need.

The icon is a rounded square in deep blue holding an edit timeline: picture and sound tracks of clips, a
transition and an orange playhead. It is drawn at 4x and downsampled for anti-aliasing.

Outputs (committed, under packaging/icons/):
  aafedit.png         1024x1024 master
  aafedit.icns        macOS bundle icon (16-1024 px)
  aafedit.ico         Windows icon (16-256 px)
  hicolor/<n>x<n>/apps/aafedit.png   Linux icon theme sizes

`check` regenerates into a temporary directory and fails if the committed PNG master differs.
"""

from __future__ import annotations

import argparse
import math
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

REPO_ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = REPO_ROOT / "packaging" / "icons"
SIZE = 1024
SCALE = 4
LINUX_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)

Color = tuple[int, int, int]


@dataclass(frozen=True)
class Clip:
    track: int
    start: float
    end: float
    color: Color


BACKGROUND_TOP: Color = (38, 52, 104)
BACKGROUND_BOTTOM: Color = (14, 20, 46)
LANE: Color = (255, 255, 255)
PICTURE: Color = (88, 156, 255)
PICTURE_ALT: Color = (130, 120, 255)
SOUND: Color = (52, 199, 140)
SOUND_ALT: Color = (40, 170, 190)
PLAYHEAD: Color = (255, 149, 0)

CLIPS = (
    Clip(0, 0.00, 0.34, PICTURE),
    Clip(0, 0.38, 0.70, PICTURE_ALT),
    Clip(0, 0.74, 1.00, PICTURE),
    Clip(1, 0.12, 0.52, PICTURE_ALT),
    Clip(2, 0.00, 0.46, SOUND),
    Clip(2, 0.50, 1.00, SOUND_ALT),
    Clip(3, 0.00, 0.22, SOUND_ALT),
    Clip(3, 0.26, 0.82, SOUND),
)


def superellipse_mask(size: int, inset: int, exponent: float = 5.0) -> Image.Image:
    """A macOS-style rounded square: |x|^n + |y|^n <= 1 inside the inset box."""
    mask = Image.new("L", (size, size), 0)
    radius = (size - 2 * inset) / 2
    centre = size / 2
    points = []
    steps = 720

    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        x = centre + radius * (abs(c) ** (2 / exponent)) * (1 if c >= 0 else -1)
        y = centre + radius * (abs(s) ** (2 / exponent)) * (1 if s >= 0 else -1)
        points.append((x, y))
    ImageDraw.Draw(mask).polygon(points, fill=255)
    return mask


def vertical_gradient(size: int, top: Color, bottom: Color) -> Image.Image:
    gradient = Image.new("RGB", (1, size))
    for y in range(size):
        t = y / (size - 1)
        gradient.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return gradient.resize((size, size))


def overlay(size: int) -> tuple[Image.Image, ImageDraw.ImageDraw]:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    return layer, ImageDraw.Draw(layer)


def lighter(color: Color, amount: int) -> Color:
    return (min(255, color[0] + amount), min(255, color[1] + amount), min(255, color[2] + amount))


def draw_icon() -> Image.Image:
    s = SIZE * SCALE
    inset = 100 * SCALE
    body = superellipse_mask(s, inset)

    canvas = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    shadow = Image.new("RGBA", (s, s), (0, 0, 0, 255))
    shadow.putalpha(body.filter(ImageFilter.GaussianBlur(16 * SCALE)).point(lambda a: round(a * 0.4)))
    canvas.alpha_composite(shadow, (0, 12 * SCALE))

    face = vertical_gradient(s, BACKGROUND_TOP, BACKGROUND_BOTTOM).convert("RGBA")
    glow, draw = overlay(s)
    draw.ellipse((-0.3 * s, -0.75 * s, 1.3 * s, 0.42 * s), fill=(130, 160, 255, 70))
    face.alpha_composite(glow.filter(ImageFilter.GaussianBlur(90 * SCALE)))

    left, right = 214 * SCALE, 810 * SCALE
    width = right - left
    lane_height = 88 * SCALE
    gap = 30 * SCALE
    top = 318 * SCALE
    radius = 20 * SCALE

    def lane_y(track: int) -> float:
        return top + track * (lane_height + gap) + (26 * SCALE if track >= 2 else 0)

    lanes, draw = overlay(s)
    for track in range(4):
        y = lane_y(track)
        draw.rounded_rectangle((left - 14 * SCALE, y - 9 * SCALE, right + 14 * SCALE, y + lane_height + 9 * SCALE), radius=radius + 7 * SCALE, fill=(255, 255, 255, 26))
    face.alpha_composite(lanes)

    clips, draw = overlay(s)
    for clip in CLIPS:
        y = lane_y(clip.track)
        x0, x1 = left + clip.start * width, left + clip.end * width
        draw.rounded_rectangle((x0, y, x1, y + lane_height), radius=radius, fill=clip.color)
        draw.rounded_rectangle((x0, y, x1, y + lane_height * 0.5), radius=radius, fill=lighter(clip.color, 26))
        draw.rectangle((x0, y + lane_height * 0.3, x1, y + lane_height * 0.5), fill=lighter(clip.color, 26))
        draw.rounded_rectangle((x0, y + lane_height * 0.5, x1, y + lane_height), radius=radius, fill=clip.color)
        draw.rectangle((x0, y + lane_height * 0.5, x1, y + lane_height * 0.7), fill=clip.color)
    face.alpha_composite(clips)

    dissolve, draw = overlay(s)
    y = lane_y(0)
    cx = left + 0.36 * width
    half = 40 * SCALE
    draw.polygon([(cx - half, y + lane_height), (cx + half, y), (cx + half, y + lane_height)], fill=(255, 255, 255, 90))
    face.alpha_composite(dissolve)

    head, draw = overlay(s)
    x = left + 0.62 * width
    line_top = 262 * SCALE
    line_bottom = lane_y(3) + lane_height + 26 * SCALE
    draw.rounded_rectangle((x - 7 * SCALE, line_top, x + 7 * SCALE, line_bottom), radius=7 * SCALE, fill=PLAYHEAD)
    w = 34 * SCALE
    draw.rounded_rectangle((x - w, line_top - 64 * SCALE, x + w, line_top - 4 * SCALE), radius=12 * SCALE, fill=PLAYHEAD)
    draw.polygon([(x - w, line_top - 20 * SCALE), (x + w, line_top - 20 * SCALE), (x, line_top + 26 * SCALE)], fill=PLAYHEAD)
    face.alpha_composite(head)

    rim = Image.new("RGBA", (s, s), (255, 255, 255, 0))
    edge = ImageChops.subtract(body, body.filter(ImageFilter.MinFilter(9)))
    rim.putalpha(edge.point(lambda a: round(a * 0.22)))
    face.alpha_composite(rim)

    face.putalpha(ImageChops.multiply(face.getchannel("A"), body))
    canvas.alpha_composite(face)
    return canvas.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def write_all(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    icon = draw_icon()
    icon.save(out / "aafedit.png", optimize=True)
    icon.save(out / "aafedit.icns", sizes=[(n, n) for n in (16, 32, 64, 128, 256, 512, 1024)])
    icon.save(out / "aafedit.ico", sizes=[(n, n) for n in ICO_SIZES])
    for n in LINUX_SIZES:
        path = out / "hicolor" / f"{n}x{n}" / "apps" / "aafedit.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        icon.resize((n, n), Image.Resampling.LANCZOS).save(path, optimize=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("step", choices=("write", "check"), default="write", nargs="?")
    args = parser.parse_args(argv)
    if args.step == "check":
        with tempfile.TemporaryDirectory() as tmp:
            write_all(Path(tmp))
            fresh = Image.open(Path(tmp) / "aafedit.png").convert("RGBA")
            committed = Image.open(OUT_DIR / "aafedit.png").convert("RGBA")
            if ImageChops.difference(fresh, committed).getbbox() is not None:
                print("packaging/icons is out of date; run tools/gen_icons.py", file=sys.stderr)
                return 1
        return 0
    write_all(OUT_DIR)
    print(f"wrote {OUT_DIR.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
