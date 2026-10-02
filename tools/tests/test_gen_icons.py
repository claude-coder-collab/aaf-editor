from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gen_icons
from PIL import Image


def test_icon_is_square_with_transparent_corners() -> None:
    icon = gen_icons.draw_icon()
    assert icon.size == (gen_icons.SIZE, gen_icons.SIZE)
    assert icon.mode == "RGBA"
    assert icon.getpixel((0, 0))[3] == 0
    assert icon.getpixel((gen_icons.SIZE // 2, gen_icons.SIZE // 2))[3] == 255


def test_every_format_is_written(tmp_path: Path) -> None:
    gen_icons.write_all(tmp_path)
    assert Image.open(tmp_path / "aafedit.icns").size == (1024, 1024)
    assert (16, 16) in Image.open(tmp_path / "aafedit.ico").info["sizes"]
    for n in gen_icons.LINUX_SIZES:
        assert Image.open(tmp_path / "hicolor" / f"{n}x{n}" / "apps" / "aafedit.png").size == (n, n)


def test_committed_icons_are_up_to_date() -> None:
    assert gen_icons.main(["check"]) == 0
